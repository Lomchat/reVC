#pragma once

// The shared world's modules talking to each other (docs/mmo.md, section 7). The hooks the game's code calls are in
// mmo.h. Every function declared here has a weak default in mmo_weak.cpp (so a module builds and runs alone); the
// module named in each section defines the real one. Names: <Module>_<What> for a module's handlers, plain names for
// the core's services.

#ifdef ORTHROS_EXTRAS
#include "common.h"
#include "mmo.h"

class CEntity;
class CPed;
class CVehicle;

namespace OrthrosMmo
{
// ================================================================ wire constants (docs/mmo.md, section 5)
enum : uint8 {
	// client -> server
	C_HELLO = 0x01, C_CHAR_CREATE, C_CHAR_SKIN, C_CHAR_DELETE, C_JOIN, C_LEAVE, C_PING, C_AWAY, C_CHAR_SAVE, C_CHAT,
	C_WORLD_SET, C_WORLD_EVENT, C_LOCK, C_STORE, C_UNLOCK, C_OWN, C_PLAYER_INFO,
	// relayed both ways (server -> client: with `from`)
	R_PLAYER_STATE = 0x20, R_ENTITY_STATE, R_ENTITY_GONE, R_EVENT, R_TARGETED,
	// server -> client
	S_REFUSED = 0x80, S_LOBBY, S_CHAR_RESULT, S_WELCOME = 0x85, S_LEFT, S_PONG, S_CHAT = 0x8A, S_WORLD, S_DENIED,
	S_WORLD_EVENT, S_RECORD, S_OWNER = 0x90, S_PLAYER_JOIN, S_PLAYER_AWAY, S_PLAYER_LEAVE, S_PLAYER_INFO,
	// page -> engine pseudo-record after a reconnection
	P_LINK_RESET = 0xFE,
};
enum { FROM_SERVER = 255, NO_SLOT = 255, MAX_SLOTS = 32 };
enum { PROTOCOL = 1 };
// ENTITY kinds and flags (R_ENTITY_STATE)
enum { KIND_PED = 1, KIND_AUTOMOBILE, KIND_BIKE, KIND_BOAT, KIND_HELI, KIND_PLANE, KIND_OBJECT };
enum { EF_DRIVEN = 1, EF_LAW = 2, EF_PARKED = 4, EF_MISSION = 8, EF_DEAD = 16 };
// EVENT / TARGETED sub ranges
enum { SUB_PLAYERS = 0x01, SUB_COMBAT = 0x20, SUB_ENTITIES = 0x40, SUB_WORLD = 0x60, SUB_SOCIAL = 0x80, SUB_SESSION = 0xA0 };
// world variables (S_WORLD) and records
enum { WV_FORCED_WEATHER = 1, WV_CHEATS = 2, WV_COUNT = 16 };
enum { RECORD_GARAGE = 0x0100 };
// world events (C_WORLD_EVENT / S_WORLD_EVENT)
enum { WE_BIGBANG = 1 };
// player network ids
static inline uint32 PlayerNetId(int slot) { return 0xFFF00000u | (uint32)slot; }
static inline bool IsPlayerNetId(uint32 id) { return (id & 0xFFF00000u) == 0xFFF00000u; }
static inline int SlotOfNetId(uint32 id) { return (int)(id & 0xFF); }

// ================================================================ records: writing and reading (header-only)
// The frame's outgoing records (core): a record is committed into the frame buffer; the buffer goes to the page at
// EndProcess / after FrontendFrame (or earlier when full). Unreliable records (states) may be dropped by the page
// under backpressure.
void CommitRecord(const uint8 *record, int32 length, bool unreliable);

class Out
{
public:
	uint8 b[1500];
	int32 n;
	bool ok, unreliable, done;
	Out(uint8 type, bool unreliable_ = false) : n(2), ok(true), unreliable(unreliable_), done(false) { b[n++] = type; }
	~Out() { End(); }
	void End() { if (done) return; done = true; if (!ok) return; b[0] = (n - 2) & 0xFF; b[1] = (n - 2) >> 8; CommitRecord(b, n, unreliable); }
	void Cancel() { done = true; }
	void Raw(const void *p, int32 k) { if (n + k > (int32)sizeof(b)) { ok = false; return; } memcpy(b + n, p, k); n += k; }
	void U8(uint32 v) { uint8 x = v; Raw(&x, 1); }
	void U16(uint32 v) { uint8 x[2] = { (uint8)v, (uint8)(v >> 8) }; Raw(x, 2); }
	void I16(int32 v) { U16((uint32)(uint16)(int16)v); }
	void U32(uint32 v) { uint8 x[4] = { (uint8)v, (uint8)(v >> 8), (uint8)(v >> 16), (uint8)(v >> 24) }; Raw(x, 4); }
	void I32(int32 v) { U32((uint32)v); }
	void F32(float v) { uint32 u; memcpy(&u, &v, 4); U32(u); }
	void F64(double v) { uint64_t u; memcpy(&u, &v, 8); U32((uint32)u); U32((uint32)(u >> 32)); }
	void Vec(const CVector &v) { F32(v.x); F32(v.y); F32(v.z); }
	void Str(const char *s) { int32 k = (int32)strlen(s); if (k > 255) k = 255; U8(k); Raw(s, k); }
};

class In
{
public:
	const uint8 *p;
	int32 n, at;
	bool ok;
	In(const uint8 *p_, int32 n_) : p(p_), n(n_), at(0), ok(true) {}
	int32 Left() const { return n - at; }
	bool Ok() const { return ok; }
	const uint8 *Raw(int32 k) { if (k < 0 || at + k > n) { ok = false; at = n; return nil; } const uint8 *r = p + at; at += k; return r; }
	uint32 U8() { const uint8 *r = Raw(1); return r ? r[0] : 0; }
	uint32 U16() { const uint8 *r = Raw(2); return r ? (uint32)r[0] | (uint32)r[1] << 8 : 0; }
	int32 I16() { return (int16)U16(); }
	uint32 U32() { const uint8 *r = Raw(4); return r ? (uint32)r[0] | (uint32)r[1] << 8 | (uint32)r[2] << 16 | (uint32)r[3] << 24 : 0; }
	int32 I32() { return (int32)U32(); }
	float F32() { uint32 u = U32(); float f; memcpy(&f, &u, 4); return f; }
	double F64() { uint64_t lo = U32(), hi = U32(); uint64_t u = lo | hi << 32; double d; memcpy(&d, &u, 8); return d; }
	CVector Vec() { float x = F32(), y = F32(), z = F32(); return CVector(x, y, z); }
	// A str8 into out (NUL-terminated, cut to cap-1); its length.
	int32 Str(char *out, int32 cap) { int32 k = U8(); const uint8 *r = Raw(k); if (!r) { out[0] = 0; return 0; } int32 m = k < cap - 1 ? k : cap - 1; memcpy(out, r, m); out[m] = 0; return m; }
};

// ================================================================ core services (mmo_wire.cpp)
// Wall-clock milliseconds (emscripten_get_now), and the server's (synced with PING/PONG).
double Now(void);
double ServerNow(void);

// The page's link.
enum { LINK_NONE, LINK_CONNECTING, LINK_OPEN, LINK_REFUSED, LINK_RETRY };
bool NetAvailable(void);   // the page offers a shared world for this game
void Connect(void);        // opens the link; HELLO is sent when it is open
void Disconnect(void);
int32 LinkState(void);
int32 Refusal(void);       // the last REFUSED reason (0 none)
void SignIn(void);         // the page's sign-in prompt
bool Testing(void);        // ?mmotest on the page

// The session's mode (set by the session module).
enum { MODE_OFF, MODE_LOBBY, MODE_JOINING, MODE_ACTIVE, MODE_LEAVING };
int32 Mode(void);
void SetMode(int32 mode);
int32 LocalSlot(void);
uint16 LocalTag(void);
uint32 LocalRank(void);
uint32 NewNetId(void);     // (tag << 20) | counter
bool LangFrench(void);
// UTF-8 to the game's text (accents at the codes its fonts use).
void GameText(const char *utf8, wchar *out, int32 size);

// The lobby (LOBBY, CHAR_RESULT).
struct LobbyCharacter { uint32 id; char name[20]; char skin[24]; int32 money; bool online; };
struct LobbyData {
	bool valid, admin;
	int32 maxCharacters, count;
	char username[40];
	LobbyCharacter chars[16];
	int32 resultOp, resultStatus; uint32 resultId; uint32 resultSerial; // the last CHAR_RESULT (serial++ each)
	uint32 serial; // ++ at each LOBBY
};
const LobbyData &Lobby(void);
void RequestCreate(const char *name, const char *skin);
void RequestSkin(uint32 id, const char *skin);
void RequestDelete(uint32 id);
void RequestJoin(uint32 id);
void RequestLeave(void);

// The joined character (WELCOME) and the world (WELCOME, WORLD).
struct Character { uint32 id; char name[20]; char skin[24]; int32 stateLength; uint8 state[4096]; };
const Character &Me(void);
void SetMySkin(const char *skin); // PLAYER_INFO
struct WorldState { bool valid; double baseServerMs, baseMinutes; float msPerMinute; int32 vars[WV_COUNT]; bool varSet[WV_COUNT]; uint32 serial; };
const WorldState &World(void);
double WorldMinutes(void); // game minutes now (day * 1440 + hour * 60 + minute, fractional)

// The other players (PLAYERINFO; the players module keeps pos/area/wanted current from their states).
struct RemotePlayer {
	bool present, admin, away;
	uint8 slot;
	uint32 rank;
	char name[20], skin[24];
	CVector pos; int32 area; uint8 wanted; double lastState; bool hasState;
	CPed *puppet;
};
RemotePlayer *Player(int32 slot); // nil when the slot is empty

// The entity registry (side tables by pool slot): every networked ped and vehicle, owned or puppet.
struct NetEntity {
	CEntity *e;
	uint32 netid;
	uint8 owner;      // slot of its owner (LocalSlot() for ours)
	uint8 kind, flags;
	bool puppet;      // a copy of another client's entity
	int8 player;      // slot of the remote player this ped is, else -1
	void *data;       // the owning module's own data (puppet buffers…)
};
NetEntity *Find(const CEntity *e);
NetEntity *FindNet(uint32 netid);
NetEntity *Register(CEntity *e, uint32 netid, int32 owner, bool puppet, uint8 kind);
void Unregister(const CEntity *e);
int32 RegistrySize(void);       // for iteration: slots 0..size-1
NetEntity *RegistryAt(int32 i); // nil for an empty slot
// The network id of an entity as the others know it: the local player (and its vehicle) included; 0 if none.
uint32 NetIdOf(const CEntity *e);
// An entity by network id (a remote player's id gives its puppet ped); nil if none here.
CEntity *EntityOf(uint32 netid);
// Simulated here (the local player, its vehicle, our owned entities, entities not networked yet).
bool OwnedHere(const CEntity *e);
// A replay of a remote event is running (explosion, death…): hooks must not send it back.
extern bool bReplaying;

// Debug JSON (?mmotest): modules append `,"key":value` fragments from their <Module>_Debug().
void DebugAppend(const char *fmt, ...);

// ================================================================ module handlers (weak defaults: mmo_weak.cpp)
// session (mmo_session.cpp)
void Session_Welcome(void);                 // WELCOME parsed (Me(), World(), players): start loading the world
void Session_Refused(int32 reason);
void Session_CharResult(int32 op, int32 status, uint32 id);
void Session_Left(void);
void Session_LinkReset(void);               // the link came back: rejoin
void Session_BeginProcess(void);
void Session_PreWorld(void);
void Session_PostWorld(void);
void Session_EndProcess(void);
void Session_FrontendFrame(void);
void Session_OnEvent(int32 from, int32 sub, In &in);
void Session_OnTargeted(int32 from, uint32 target, int32 sub, In &in);
bool Session_TestCommand(const char *cmd);
void Session_Debug(void);
void Session_PlayerDied(void);
void Session_PlayerRespawned(void);
// lobby (mmo_lobby.cpp)
void Lobby_Changed(void);                   // LOBBY or CHAR_RESULT received, link state changed
void Lobby_FrontendFrame(void);
bool Lobby_KeyDown(int32 rsKey);
void Lobby_TextInput(const char *utf8);
wchar *Lobby_Text(const char *key);         // dynamic "ORT_*" texts of its pages (nil: not one)
void Lobby_ScreenChanged(int32 screen);
void Lobby_DrawFrontend(int32 screen);
bool Lobby_TestCommand(const char *cmd);
void Lobby_Debug(void);
// players (mmo_players.cpp)
void Players_OnState(int32 from, In &in);   // in at the record's body start (x, y, z first)
void Players_Joined(int32 slot);
void Players_Left(int32 slot);
void Players_Info(int32 slot);
void Players_Away(int32 slot);
void Players_GameLoaded(void);
void Players_PreWorld(void);
void Players_PostWorld(void);
void Players_Render2d(void);
bool Players_PuppetControl(CPed *ped, NetEntity *ne);
void Players_Reset(void);                   // the world left or the link reset: every remote puppet removed
void Players_OnEvent(int32 from, int32 sub, In &in);
void Players_OnTargeted(int32 from, uint32 target, int32 sub, In &in);
bool Players_TestCommand(const char *cmd);
void Players_Debug(void);
// entities (mmo_entities.cpp)
void Entities_OnState(int32 from, In &in);  // in at the record's body start (netid first)
void Entities_OnGone(int32 from, uint32 netid, int32 reason);
void Entities_OnOwner(uint32 netid, int32 owner, int32 previous);
void Entities_GameLoaded(void);
void Entities_PreWorld(void);
void Entities_PostWorld(void);
void Entities_Reset(void);
void Entities_OnEvent(int32 from, int32 sub, In &in);
void Entities_OnTargeted(int32 from, uint32 target, int32 sub, In &in);
bool Entities_TestCommand(const char *cmd);
void Entities_Debug(void);
// combat (mmo_combat.cpp)
void Combat_PreWorld(void);
void Combat_PostWorld(void);
void Combat_Reset(void);
void Combat_OnEvent(int32 from, int32 sub, In &in);
void Combat_OnTargeted(int32 from, uint32 target, int32 sub, In &in);
bool Combat_TestCommand(const char *cmd);
void Combat_Debug(void);
void Combat_PlayerDied(void);
void Combat_PlayerRespawned(void);
// world (mmo_world.cpp)
void World_Changed(void);                   // WORLD received (or WELCOME's)
void World_OnWorldEvent(int32 from, int32 id, int32 arg);
void World_OnRecord(uint16 key, int32 holder, const uint8 *data, int32 length);
void World_GameLoaded(void);
void World_BeginProcess(void);
void World_PreWorld(void);
void World_Reset(void);
void World_OnEvent(int32 from, int32 sub, In &in);
bool World_TestCommand(const char *cmd);
void World_Debug(void);
// A cheat typed by an admin (its code as the HELP list shows it, e.g. "ALOVELYDAY"): true = a world cheat, sent to
// the server (not applied here now); false = a personal one, the game's own code applies it.
bool World_CheatRequested(const char *code);
// social (mmo_social.cpp)
void Social_OnChat(int32 from, int32 kind, const char *name, const char *text);
void Social_OnDenied(int32 type, int32 reason);
// A line in the chat area: kinds 0 chat, 1 notice (joins, leaves), 2 kill, 3 error. UTF-8.
enum { LINE_CHAT, LINE_NOTICE, LINE_KILL, LINE_ERROR };
void Social_AddLine(int32 kind, const char *text);
void Social_BeginProcess(void);
void Social_RenderOverlay(void);
bool Social_KeyDown(int32 rsKey);
void Social_TextInput(const char *utf8);
bool Social_OverlayHasMouse(void);
void Social_OverlayMouse(float x, float y);
bool Social_OverlayClick(float x, float y);
void Social_Reset(void);
void Social_OnEvent(int32 from, int32 sub, In &in);
bool Social_TestCommand(const char *cmd);
void Social_Debug(void);
}
#endif
