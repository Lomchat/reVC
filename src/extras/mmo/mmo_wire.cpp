// The shared world's core (docs/mmo.md, sections 5 to 7): the page's link (EM_JS, Module.orthrosNet), the frame's
// records sent at its end, the page's queue drained and every record dispatched to its module, the link's states
// (HELLO once it is open; a reconnection or a lost queue: the modules reset, HELLO again, the session rejoins), the
// server's clock, the lobby, the joined character, the world, the other players, the game's hooks fanned out to the
// modules, the test commands and the debug state. The entity registry: mmo_registry.cpp.
// Nothing here costs anything while no shared world is wanted: every hook returns at once (Dormant()).
#include "common.h"

#ifdef ORTHROS_EXTRAS
#include <stdarg.h>
#include "mmo_internal.h"
#include "orthros_extras.h"
#include "Frontend.h"
#include "PlayerPed.h"
#include "PlayerInfo.h"
#include "Timer.h"
#include "rwcore.h"
#include "skeleton.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// ---------------------------------------------------------------- the page's link
// (src/host/web/mmo-link.js through the re3 page: Module.orthrosNet. Every call returns at once; a call that throws is
// as if the page had no link, its first error logged)
#ifdef __EMSCRIPTEN__
EM_JS_DEPS(orthros_mmo, "$UTF8ToString,$stringToUTF8");

EM_JS(int, OrthrosNetAvailable, (), { return Module.orthrosNet ? 1 : 0; });

EM_JS(void, OrthrosNetOpen, (), {
	try { Module.orthrosNet.open(); } catch (e) { if (!Module.orthrosNetFailed) console.error('orthrosNet.open', e); Module.orthrosNetFailed = true; }
});

EM_JS(void, OrthrosNetClose, (), {
	try { Module.orthrosNet.close(); } catch (e) { if (!Module.orthrosNetFailed) console.error('orthrosNet.close', e); Module.orthrosNetFailed = true; }
});

EM_JS(int, OrthrosNetState, (), {
	try { return Module.orthrosNet.state() | 0; } catch (e) { return 0; }
});

EM_JS(int, OrthrosNetRefusal, (), {
	try { return Module.orthrosNet.refusal() | 0; } catch (e) { return 0; }
});

EM_JS(int, OrthrosNetSend, (const unsigned char *data, int length, int unreliable), {
	try {
		data >>>= 0;
		return Module.orthrosNet.send(HEAPU8.slice(data, data + length), !!unreliable) ? 1 : 0;
	} catch (e) { if (!Module.orthrosNetFailed) console.error('orthrosNet.send', e); Module.orthrosNetFailed = true; return 0; }
});

EM_JS(int, OrthrosNetDrain, (unsigned char *buffer, int capacity), {
	try { return Module.orthrosNet.drain(HEAPU8, buffer >>> 0, capacity) | 0; }
	catch (e) { if (!Module.orthrosNetFailed) console.error('orthrosNet.drain', e); Module.orthrosNetFailed = true; return 0; }
});

EM_JS(int, OrthrosNetUser, (char *buffer, int capacity), {
	try {
		var n = Module.orthrosNet, u = typeof n.user === 'function' ? n.user() : n.user;
		u = u ? String(u) : '';
		stringToUTF8(u, buffer >>> 0, capacity);
		return u.length ? 1 : 0;
	} catch (e) { return 0; }
});

EM_JS(void, OrthrosNetSignIn, (), {
	try { Module.orthrosNet.signIn(); } catch (e) { if (!Module.orthrosNetFailed) console.error('orthrosNet.signIn', e); Module.orthrosNetFailed = true; }
});

EM_JS(int, OrthrosNetTesting, (), {
	try { var n = Module.orthrosNet, t = typeof n.testing === 'function' ? n.testing() : n.testing; return t ? 1 : 0; } catch (e) { return 0; }
});

EM_JS(int, OrthrosNetTestCommand, (char *buffer, int capacity), {
	try {
		var q = Module.orthrosNet.testCommands;
		if (!q || !q.length) return 0;
		stringToUTF8(String(q.shift()), buffer >>> 0, capacity);
		return 1;
	} catch (e) { return 0; }
});

EM_JS(void, OrthrosNetDebug, (const char *json), {
	try { var n = Module.orthrosNet; if (n.debug) n.debug(UTF8ToString(json >>> 0)); }
	catch (e) { if (!Module.orthrosNetFailed) console.error('orthrosNet.debug', e); Module.orthrosNetFailed = true; }
});
#else
// (a build without the browser: no page, no shared world)
static int OrthrosNetAvailable(void) { return 0; }
static void OrthrosNetOpen(void) {}
static void OrthrosNetClose(void) {}
static int OrthrosNetState(void) { return 0; }
static int OrthrosNetRefusal(void) { return 0; }
static int OrthrosNetSend(const unsigned char *, int, int) { return 0; }
static int OrthrosNetDrain(unsigned char *, int) { return 0; }
static int OrthrosNetUser(char *, int) { return 0; }
static void OrthrosNetSignIn(void) {}
static int OrthrosNetTesting(void) { return 0; }
static int OrthrosNetTestCommand(char *, int) { return 0; }
static void OrthrosNetDebug(const char *) {}
#endif

namespace OrthrosMmo
{
// ---------------------------------------------------------------- state
static bool pageChecked, pageLink, testing;
static double nextPageCheck;
static bool linkWanted;              // (from Connect to Disconnect)
static int32 linkState = LINK_NONE;  // (as last polled)
static int32 refusal;
static char refusalDetail[128];
static bool helloSent;               // (HELLO committed on this connection: the records may go)
static bool helloThisPump;
static int32 mode = MODE_OFF;
static int32 localSlot = NO_SLOT;
static uint16 localTag;
static uint32 localRank;
static uint32 netCounter;
static bool admin;
static LobbyData lobby;
static Character me;
static WorldState world;
static RemotePlayer players[MAX_SLOTS];

// (the server's clock: the last PONGs' round trips and offsets; the one of the shortest round trip is the best)
enum { NUM_SAMPLES = 8 };
static struct { double rtt, offset; } samples[NUM_SAMPLES];
static int32 numSamples, nextSample;
static double serverOffset, bestRtt;
static bool haveOffset;
static double nextPing;
static bool awaySent;

// (the frame's records, reliable [0] and unreliable [1])
enum { OUT_CAP = 64 * 1024 };
static uint8 outBuf[2][OUT_CAP];
static int32 outLen[2];

// (the page's messages drained and not yet dispatched, [inboxAt, inboxLen): u32 length | message, whole ones)
static uint8 inbox[1 << 20];
static int32 inboxAt, inboxLen;
static bool dispatching, dropPending, overflowPending;

// (test commands and the debug state)
static int32 keyHeld = -1, keyPumps, gapPumps;
static double keyUpAt;
static double nextDebug, lastDebug;
static int32 gameFrames;
static char debugBuf[64 * 1024];
static int32 debugLen;
static bool debugOverflow;

static void SendHello(void);

// (no shared world wanted: every hook returns at once)
static inline bool
Dormant(void)
{
	return mode == MODE_OFF && !linkWanted && linkState == LINK_NONE;
}

static void
Copy(char *out, const char *s, int32 size)
{
	strncpy(out, s ? s : "", size - 1);
	out[size - 1] = '\0';
}

// ---------------------------------------------------------------- time
double
Now(void)
{
#ifdef __EMSCRIPTEN__
	return emscripten_get_now();
#else
	return (double)CTimer::GetCurrentTimeInCycles() / CTimer::GetCyclesPerMillisecond();
#endif
}

double
ServerNow(void)
{
	return Now() + serverOffset;
}

// ---------------------------------------------------------------- the page
// (checked at the first frame, then every second until there is one)
static bool
PageLink(void)
{
	if (pageLink)
		return true;
	const double now = Now();
	if (pageChecked && now < nextPageCheck)
		return false;
	pageChecked = true;
	nextPageCheck = now + 1000.0;
	pageLink = OrthrosNetAvailable() != 0;
	if (pageLink) {
		testing = OrthrosNetTesting() != 0;
		debug("Orthros MMO: the page offers a shared world%s\n", testing ? " (testing)" : "");
	}
	return pageLink;
}

bool
NetAvailable(void)
{
	pageChecked = false;
	return PageLink();
}

bool
Testing(void)
{
	return PageLink() && testing;
}

void
SignIn(void)
{
	if (PageLink())
		OrthrosNetSignIn();
}

int32
LinkState(void)
{
	return linkState;
}

int32
Refusal(void)
{
	return refusal;
}

// ---------------------------------------------------------------- outgoing records
static void
DropOut(void)
{
	outLen[0] = outLen[1] = 0;
}

static bool HasReset(const uint8 *p, int32 n);

// (the page's queue read ahead into the inbox, dispatched by the next Pump: true if a reconnection is in it)
static bool
PeekReset(void)
{
	if (!dispatching && inboxAt >= inboxLen)
		inboxAt = inboxLen = 0;
	const int32 room = (int32)sizeof(inbox) - inboxLen;
	if (room < 64 * 1024)
		return false;
	const int32 n = OrthrosNetDrain(inbox + inboxLen, room);
	if (n < 0) {
		overflowPending = true;
		return false;
	}
	const int32 from = inboxLen;
	inboxLen += n;
	return HasReset(inbox + from, n);
}

// (what the page's queue holds is not to be dispatched: an earlier link's. Deferred while a record is being dispatched)
static void
DropPage(void)
{
	if (dispatching) {
		dropPending = true;
		return;
	}
	dropPending = false;
	for (int32 i = 0; i < 64 && OrthrosNetDrain(inbox, sizeof(inbox)) != 0; i++)
		;
	inboxAt = inboxLen = 0;
	overflowPending = false;
}

// (may the frame's records go now? The link open, and no reconnection waiting in the page's queue: records sent then
// would reach the new connection before its HELLO. When not, they are dropped)
static bool
CanSend(void)
{
	if (!helloSent)
		return false;
	if (OrthrosNetState() != LINK_OPEN || PeekReset()) {
		helloSent = false;
		DropOut();
		return false;
	}
	return true;
}

static void
SendOut(int32 k)
{
	if (outLen[k] > 0 && CanSend())
		OrthrosNetSend(outBuf[k], outLen[k], k);
	outLen[k] = 0;
}

static void
Flush(void)
{
	if (outLen[0] == 0 && outLen[1] == 0)
		return;
	if (!CanSend()) {
		DropOut();
		return;
	}
	if (outLen[0] > 0)
		OrthrosNetSend(outBuf[0], outLen[0], 0);
	if (outLen[1] > 0)
		OrthrosNetSend(outBuf[1], outLen[1], 1);
	DropOut();
}

// (a record goes once HELLO opened the connection: one committed before is dropped)
void
CommitRecord(const uint8 *record, int32 length, bool unreliable)
{
	if (!helloSent || length <= 0)
		return;
	const int32 k = unreliable ? 1 : 0;
	if (outLen[k] + length > OUT_CAP) {
		SendOut(k);
		if (!helloSent)
			return;
		if (length > OUT_CAP) {
			if (CanSend())
				OrthrosNetSend(record, length, k);
			return;
		}
	}
	memcpy(outBuf[k] + outLen[k], record, length);
	outLen[k] += length;
}

// ---------------------------------------------------------------- the link
static void
ClearPlayer(RemotePlayer &p)
{
	p.present = p.admin = p.away = false;
	p.slot = NO_SLOT;
	p.rank = 0;
	p.name[0] = p.skin[0] = '\0';
	p.pos = CVector(0.0f, 0.0f, 0.0f);
	p.area = 0;
	p.wanted = 0;
	p.lastState = 0.0;
	p.hasState = false;
	p.puppet = nil;
}

static void
ClearPlayers(void)
{
	for (int32 i = 0; i < MAX_SLOTS; i++)
		ClearPlayer(players[i]);
}

// (the world's session over: no slot, no tag, no other players — their puppets are the players module's to remove,
// Players_Reset, before)
static void
LeftWorld(void)
{
	ClearPlayers();
	localSlot = NO_SLOT;
	localTag = 0;
	localRank = 0;
}

void
Connect(void)
{
	if (!NetAvailable())
		return;
	const int32 st = OrthrosNetState();
	if (st == LINK_NONE || st == LINK_REFUSED) {
		// (a new link: what an earlier one left in the page's queue is not its own, nor its players)
		DropPage();
		refusal = 0;
		refusalDetail[0] = '\0';
		helloSent = false;
		DropOut();
		LeftWorld();
	}
	linkWanted = true;
	OrthrosNetOpen();
}

void
Disconnect(void)
{
	if (!pageLink)
		return;
	Flush(); // (the last reliable records: LEAVE, CHAR_SAVE)
	OrthrosNetClose();
	linkWanted = false;
	helloSent = false;
	DropOut();
	DropPage();
	LeftWorld();
	lobby.valid = false;
	linkState = OrthrosNetState();
}

static void
SendHello(void)
{
	helloSent = true;
	helloThisPump = true;
	awaySent = false;
	numSamples = nextSample = 0;
	nextPing = 0.0;
	Out o(C_HELLO);
	o.U16(PROTOCOL);
	o.U8(LangFrench() ? 1 : 0);
}

static void
PollLink(void)
{
	const int32 st = OrthrosNetState();
	const bool changed = st != linkState;
	if (changed) {
		debug("Orthros MMO: link %d -> %d\n", linkState, st);
		linkState = st;
		if (st != LINK_OPEN) {
			helloSent = false;
			DropOut();
		}
		if (st == LINK_REFUSED) {
			const int32 r = OrthrosNetRefusal();
			if (r != 0)
				refusal = r;
		}
		if (st == LINK_NONE || st == LINK_REFUSED)
			lobby.valid = false;
	}
	if (st == LINK_OPEN && !helloSent && linkWanted)
		SendHello();
	if (changed)
		Lobby_Changed();
}

// (the queue lost, -1 from the page, or a reconnection, LINK_RESET: what the modules hold of the others is dropped, the
// server greeted again, the session rejoins)
static void
Resync(bool reconnected)
{
	debug("Orthros MMO: %s, resynchronising\n", reconnected ? "link reset" : "queue overflow");
	Players_Reset();
	Entities_Reset();
	Combat_Reset();
	LeftWorld();
	if (!helloThisPump) {
		if (reconnected) {
			helloSent = false;
			DropOut();
		}
		if (linkState == LINK_OPEN && linkWanted)
			SendHello();
	}
	Session_LinkReset();
}

// ---------------------------------------------------------------- the clock
static void
OnPong(In &in)
{
	const double sent = in.F64(), server = in.F64();
	if (!in.Ok())
		return;
	const double now = Now(), rtt = now - sent;
	if (!(rtt >= 0.0 && rtt < 30000.0)) // (not one of ours: another clock)
		return;
	samples[nextSample].rtt = rtt;
	samples[nextSample].offset = server - (sent + now) * 0.5;
	nextSample = (nextSample + 1) % NUM_SAMPLES;
	if (numSamples < NUM_SAMPLES)
		numSamples++;
	int32 best = 0;
	for (int32 i = 1; i < numSamples; i++)
		if (samples[i].rtt < samples[best].rtt)
			best = i;
	serverOffset = samples[best].offset;
	bestRtt = samples[best].rtt;
	haveOffset = true;
}

// (WORLD body: f64 baseServerMs, f64 baseMinutes, f32 msPerMinute, u8 n, n x { u8 id, i32 value }; the variables
// named are set, the others kept — a new world's all cleared first)
static bool
ReadWorld(In &in, bool fresh)
{
	WorldState w = world;
	if (fresh)
		for (int32 i = 0; i < WV_COUNT; i++) {
			w.vars[i] = 0;
			w.varSet[i] = false;
		}
	w.baseServerMs = in.F64();
	w.baseMinutes = in.F64();
	const float rate = in.F32();
	const int32 n = in.U8();
	for (int32 i = 0; i < n && in.Ok(); i++) {
		const int32 id = in.U8(), value = in.I32();
		if (in.Ok() && id < WV_COUNT) {
			w.vars[id] = value;
			w.varSet[id] = true;
		}
	}
	if (!in.Ok())
		return false;
	w.msPerMinute = rate > 0.0f ? rate : 1000.0f;
	w.valid = true;
	w.serial = world.serial + 1;
	world = w;
	return true;
}

const WorldState &
World(void)
{
	return world;
}

double
WorldMinutes(void)
{
	if (!world.valid)
		return 720.0;
	return world.baseMinutes + (ServerNow() - world.baseServerMs) / world.msPerMinute;
}

// ---------------------------------------------------------------- the session's mode, the joined character
int32
Mode(void)
{
	return mode;
}

void
SetMode(int32 m)
{
	if (m != mode)
		debug("Orthros MMO: mode %d -> %d\n", mode, m);
	mode = m;
}

// (from the moment WELCOME starts the world's load: its first script tick, inside the load, already runs the shared
// world's rules)
bool
IsActive(void)
{
	return mode == MODE_JOINING || mode == MODE_ACTIVE || mode == MODE_LEAVING;
}

bool
IsEntering(void)
{
	return mode == MODE_JOINING;
}

bool
IsAdmin(void)
{
	return admin;
}

int32
LocalSlot(void)
{
	return localSlot;
}

uint16
LocalTag(void)
{
	return localTag;
}

uint32
LocalRank(void)
{
	return localRank;
}

uint32
NewNetId(void)
{
	if (localTag == 0)
		return 0;
	// (an id still in the registry after the counter wrapped is skipped)
	for (int32 i = 0; i < 64; i++) {
		netCounter = netCounter >= 0xFFFFF ? 1 : netCounter + 1;
		const uint32 id = (uint32)localTag << 20 | netCounter;
		if (FindNet(id) == nil)
			return id;
	}
	return (uint32)localTag << 20 | netCounter;
}

bool
LangFrench(void)
{
	return FrontEndMenuManager.m_PrefsLanguage == CMenuManager::LANGUAGE_FRENCH;
}

void
GameText(const char *utf8, wchar *out, int32 size)
{
	OrthrosExtras::GameText(utf8, out, size);
}

const Character &
Me(void)
{
	return me;
}

void
SetMySkin(const char *skin)
{
	if (skin != me.skin)
		Copy(me.skin, skin, sizeof(me.skin));
	Out o(C_PLAYER_INFO);
	o.Str(me.skin);
}

// ---------------------------------------------------------------- the lobby
const LobbyData &
Lobby(void)
{
	return lobby;
}

void
RequestCreate(const char *name, const char *skin)
{
	Out o(C_CHAR_CREATE);
	o.Str(name);
	o.Str(skin);
}

void
RequestSkin(uint32 id, const char *skin)
{
	Out o(C_CHAR_SKIN);
	o.U32(id);
	o.Str(skin);
}

void
RequestDelete(uint32 id)
{
	Out o(C_CHAR_DELETE);
	o.U32(id);
}

void
RequestJoin(uint32 id)
{
	Out o(C_JOIN);
	o.U32(id);
}

void
RequestLeave(void)
{
	Out o(C_LEAVE);
}

static void
OnLobby(In &in)
{
	LobbyData l;
	memset(&l, 0, sizeof(l));
	l.admin = in.U8() != 0;
	l.maxCharacters = in.U8();
	in.Str(l.username, sizeof(l.username));
	const int32 n = in.U8();
	for (int32 i = 0; i < n && in.Ok(); i++) {
		LobbyCharacter c;
		c.id = in.U32();
		in.Str(c.name, sizeof(c.name));
		in.Str(c.skin, sizeof(c.skin));
		c.money = in.I32();
		c.online = in.U8() != 0;
		if (in.Ok() && l.count < (int32)ARRAY_SIZE(l.chars))
			l.chars[l.count++] = c;
	}
	if (!in.Ok())
		return;
	l.valid = true;
	l.resultOp = lobby.resultOp;
	l.resultStatus = lobby.resultStatus;
	l.resultId = lobby.resultId;
	l.resultSerial = lobby.resultSerial;
	l.serial = lobby.serial + 1;
	lobby = l;
	admin = l.admin;
	Lobby_Changed();
}

static void
OnCharResult(In &in)
{
	const int32 op = in.U8(), status = in.U8();
	const uint32 id = in.U32();
	if (!in.Ok())
		return;
	lobby.resultOp = op;
	lobby.resultStatus = status;
	lobby.resultId = id;
	lobby.resultSerial++;
	Session_CharResult(op, status, id);
	Lobby_Changed();
}

// ---------------------------------------------------------------- the other players
RemotePlayer *
Player(int32 slot)
{
	return slot >= 0 && slot < MAX_SLOTS && players[slot].present ? &players[slot] : nil;
}

// (PLAYERINFO = u8 slot, u32 rank, u8 admin, u8 away, str8 name, str8 skin)
static bool
ReadPlayerInfo(In &in, RemotePlayer &p)
{
	ClearPlayer(p);
	p.slot = in.U8();
	p.rank = in.U32();
	p.admin = in.U8() != 0;
	p.away = in.U8() != 0;
	in.Str(p.name, sizeof(p.name));
	in.Str(p.skin, sizeof(p.skin));
	p.present = true;
	return in.Ok() && p.slot < MAX_SLOTS;
}

static void
Notice(const char *name, const char *en, const char *fr)
{
	char line[96];
	snprintf(line, sizeof(line), "%s %s", name, LangFrench() ? fr : en);
	Social_AddLine(LINE_NOTICE, line);
}

static void
OnPlayerJoin(In &in)
{
	RemotePlayer p;
	if (!ReadPlayerInfo(in, p))
		return;
	if (p.slot == localSlot) {
		localRank = p.rank;
		return;
	}
	if (players[p.slot].present) // (a slot given again without its leave)
		Players_Left(p.slot);
	players[p.slot] = p;
	Players_Joined(p.slot);
	Notice(p.name, "joined", "a rejoint la partie");
}

static void
OnPlayerLeave(In &in)
{
	const int32 slot = in.U8();
	RemotePlayer *p = in.Ok() && slot != localSlot ? Player(slot) : nil;
	if (p == nil)
		return;
	// (told while the slot still holds its name and puppet)
	Players_Left(slot);
	Notice(p->name, "left", "a quitté la partie");
	ClearPlayer(*p);
}

static void
OnPlayerAway(In &in)
{
	const int32 slot = in.U8();
	const bool away = in.U8() != 0;
	RemotePlayer *p = in.Ok() && slot != localSlot ? Player(slot) : nil;
	if (p == nil)
		return;
	p->away = away;
	Players_Away(slot);
}

static void
OnPlayerInfo(In &in)
{
	const int32 slot = in.U8();
	char skin[24];
	in.Str(skin, sizeof(skin));
	RemotePlayer *p = in.Ok() && slot != localSlot ? Player(slot) : nil;
	if (p == nil)
		return;
	Copy(p->skin, skin, sizeof(p->skin));
	Players_Info(slot);
}

static void
OnPlayerState(int32 from, const uint8 *body, int32 length)
{
	RemotePlayer *p = from != localSlot ? Player(from) : nil;
	if (p == nil)
		return;
	In head(body, length);
	const CVector pos = head.Vec();
	if (!head.Ok() || !PositionInWorld(pos)) // (NaN or out of the world: the whole state dropped)
		return;
	p->pos = pos;
	p->hasState = true;
	p->lastState = Now();
	In in(body, length);
	Players_OnState(from, in);
}

// (u8 slot, u16 tag, f64 serverMs, u8 admin, u32 charId, str8 name, str8 skin, u16 n, n bytes state, WORLD body,
// u8 nPlayers, nPlayers x PLAYERINFO)
static void
OnWelcome(In &in)
{
	const int32 slot = in.U8();
	const uint16 tag = in.U16();
	const double serverMs = in.F64();
	const bool isAdmin = in.U8() != 0;
	const uint32 id = in.U32();
	char name[20], skin[24];
	in.Str(name, sizeof(name));
	in.Str(skin, sizeof(skin));
	const int32 n = in.U16();
	const uint8 *state = in.Raw(n);
	if (!in.Ok())
		return;
	me.id = id;
	Copy(me.name, name, sizeof(me.name));
	Copy(me.skin, skin, sizeof(me.skin));
	me.stateLength = Min(n, (int32)sizeof(me.state));
	memcpy(me.state, state, me.stateLength);
	ReadWorld(in, true);
	if (!haveOffset)
		serverOffset = serverMs - Now();

	LeftWorld();
	localSlot = slot;
	localTag = tag;
	admin = isAdmin;
	netCounter = 0;
	awaySent = false; // (a new session in the world: the pause menu's state told again)
	bool rankKnown = false;
	uint32 highest = 0;
	const int32 count = in.U8();
	for (int32 i = 0; i < count; i++) {
		RemotePlayer p;
		if (!ReadPlayerInfo(in, p))
			break;
		if (p.slot == slot) {
			localRank = p.rank;
			rankKnown = true;
			continue;
		}
		highest = Max(highest, p.rank);
		players[p.slot] = p;
	}
	// (not in the list: the last one in, the most junior)
	if (!rankKnown)
		localRank = highest > 0 ? highest + 1 : 0;
	debug("Orthros MMO: welcome, slot %d, tag %d, %s\n", slot, tag, me.name);

	World_Changed();
	for (int32 i = 0; i < MAX_SLOTS; i++)
		if (players[i].present)
			Players_Joined(i);
	Session_Welcome();
}

// ---------------------------------------------------------------- dispatch
static void
OnEvent(int32 from, int32 sub, In &in)
{
	if (sub >= SUB_SESSION + 0x20 || sub < SUB_PLAYERS)
		return;
	else if (sub >= SUB_SESSION)
		Session_OnEvent(from, sub, in);
	else if (sub >= SUB_SOCIAL)
		Social_OnEvent(from, sub, in);
	else if (sub >= SUB_WORLD)
		World_OnEvent(from, sub, in);
	else if (sub >= SUB_ENTITIES)
		Entities_OnEvent(from, sub, in);
	else if (sub >= SUB_COMBAT)
		Combat_OnEvent(from, sub, in);
	else
		Players_OnEvent(from, sub, in);
}

static void
OnTargeted(int32 from, uint32 target, int32 sub, In &in)
{
	if (sub >= SUB_SESSION + 0x20 || sub < SUB_PLAYERS)
		return;
	else if (sub >= SUB_SESSION)
		Session_OnTargeted(from, target, sub, in);
	else if (sub >= SUB_WORLD) // (the world's and the social module's ranges have no targeted records)
		return;
	else if (sub >= SUB_ENTITIES)
		Entities_OnTargeted(from, target, sub, in);
	else if (sub >= SUB_COMBAT)
		Combat_OnTargeted(from, target, sub, in);
	else
		Players_OnTargeted(from, target, sub, in);
}

static void
DispatchRecord(int32 type, int32 from, const uint8 *body, int32 length)
{
	In in(body, length);
	switch (type) {
	case R_PLAYER_STATE:
		OnPlayerState(from, body, length);
		break;
	case R_ENTITY_STATE:
		Entities_OnState(from, in);
		break;
	case R_ENTITY_GONE: {
		const uint32 netid = in.U32();
		const int32 reason = in.U8();
		if (in.Ok())
			Entities_OnGone(from, netid, reason);
		break;
	}
	case R_EVENT: {
		const int32 sub = in.U8();
		if (in.Ok()) {
			In rest(body + 1, length - 1);
			OnEvent(from, sub, rest);
		}
		break;
	}
	case R_TARGETED: {
		const uint32 target = in.U32();
		const int32 sub = in.U8();
		if (in.Ok()) {
			In rest(body + 5, length - 5);
			OnTargeted(from, target, sub, rest);
		}
		break;
	}
	case S_REFUSED: {
		const int32 reason = in.U8();
		in.Str(refusalDetail, sizeof(refusalDetail));
		debug("Orthros MMO: refused (%d) %s\n", reason, refusalDetail);
		refusal = reason;
		lobby.valid = false;
		Session_Refused(reason);
		Lobby_Changed();
		break;
	}
	case S_LOBBY:
		OnLobby(in);
		break;
	case S_CHAR_RESULT:
		OnCharResult(in);
		break;
	case S_WELCOME:
		OnWelcome(in);
		break;
	case S_LEFT:
		// (the session first: it removes what the world held; the core forgets its players after)
		Session_Left();
		LeftWorld();
		break;
	case S_PONG:
		OnPong(in);
		break;
	case S_CHAT: {
		char name[32], text[256];
		const int32 kind = in.U8();
		in.Str(name, sizeof(name));
		in.Str(text, sizeof(text));
		if (in.Ok())
			Social_OnChat(from, kind, name, text);
		break;
	}
	case S_WORLD:
		if (ReadWorld(in, false))
			World_Changed();
		break;
	case S_DENIED: {
		const int32 what = in.U8(), reason = in.U8();
		if (in.Ok())
			Social_OnDenied(what, reason);
		break;
	}
	case S_WORLD_EVENT: {
		const int32 id = in.U8(), arg = in.I32();
		if (in.Ok())
			World_OnWorldEvent(from, id, arg);
		break;
	}
	case S_RECORD: {
		const uint16 key = in.U16();
		const int32 holder = in.U8(), n = in.U16();
		const uint8 *data = in.Raw(n);
		if (in.Ok())
			World_OnRecord(key, holder, data, n);
		break;
	}
	case S_OWNER: {
		const uint32 netid = in.U32();
		const int32 owner = in.U8(), previous = in.U8();
		if (in.Ok())
			Entities_OnOwner(netid, owner, previous);
		break;
	}
	case S_PLAYER_JOIN:
		OnPlayerJoin(in);
		break;
	case S_PLAYER_AWAY:
		OnPlayerAway(in);
		break;
	case S_PLAYER_LEAVE:
		OnPlayerLeave(in);
		break;
	case S_PLAYER_INFO:
		OnPlayerInfo(in);
		break;
	case P_LINK_RESET:
		Resync(true);
		break;
	default: // (a newer server's: ignored)
		break;
	}
}

// (a server message: one or more records, u16 len | u8 type | u8 from | body, len counting type, from and body)
static void
DispatchMessage(const uint8 *p, int32 n)
{
	for (int32 at = 0; at + 2 <= n && !dropPending; ) {
		const int32 len = p[at] | p[at + 1] << 8;
		if (len < 2 || at + 2 + len > n)
			break;
		DispatchRecord(p[at + 2], p[at + 3], p + at + 4, len - 2);
		at += 2 + len;
	}
}

static bool
HasReset(const uint8 *p, int32 n)
{
	for (int32 at = 0; at + 4 <= n; ) {
		const uint32 len = p[at] | p[at + 1] << 8 | p[at + 2] << 16 | (uint32)p[at + 3] << 24;
		at += 4;
		if (len > (uint32)(n - at))
			break;
		for (int32 r = at, end = at + (int32)len; r + 3 <= end; ) {
			const int32 rl = p[r] | p[r + 1] << 8;
			if (rl < 2)
				break;
			if (p[r + 2] == P_LINK_RESET)
				return true;
			r += 2 + rl;
		}
		at += len;
	}
	return false;
}

// (the page's queue drained until empty, every message dispatched in order; handlers may send, and a send may read the
// queue ahead into the inbox's end, never over what is being dispatched)
static void
Receive(void)
{
	for (int32 round = 0; round < 64; round++) {
		if (dropPending)
			DropPage();
		if (overflowPending) {
			overflowPending = false;
			inboxAt = inboxLen = 0;
			Resync(false);
		}
		if (inboxAt >= inboxLen) {
			inboxAt = inboxLen = 0;
			const int32 n = OrthrosNetDrain(inbox, sizeof(inbox));
			if (n < 0) {
				overflowPending = true;
				continue;
			}
			if (n == 0)
				return;
			inboxLen = n;
		}
		dispatching = true;
		while (inboxAt + 4 <= inboxLen && !dropPending) {
			const uint8 *m = inbox + inboxAt;
			const uint32 len = m[0] | m[1] << 8 | m[2] << 16 | (uint32)m[3] << 24;
			if (len > (uint32)(inboxLen - inboxAt - 4)) {
				inboxAt = inboxLen;
				break;
			}
			inboxAt += 4 + len;
			DispatchMessage(m + 4, len);
		}
		dispatching = false;
	}
}

// (the pause menu while playing: away for the others, AWAY source 0)
static void
TrackAway(void)
{
	const bool away = mode == MODE_ACTIVE && FrontEndMenuManager.m_bMenuActive;
	if (away == awaySent)
		return;
	awaySent = away;
	Out o(C_AWAY);
	o.U8(0);
	o.U8(away ? 1 : 0);
}

// ---------------------------------------------------------------- test commands (?mmotest)
// (a word, whatever its letters' case)
static bool
SameWord(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if ((*a >= 'a' && *a <= 'z' ? *a - 'a' + 'A' : *a) != (*b >= 'a' && *b <= 'z' ? *b - 'a' + 'A' : *b))
			return false;
	return *a == *b;
}

static int32
KeyCode(const char *s)
{
	static const struct { const char *name; int32 code; } names[] = {
		{ "ESC", rsESC }, { "ENTER", rsENTER }, { "RETURN", rsENTER }, { "PADENTER", rsPADENTER }, { "BACKSP", rsBACKSP },
		{ "BACKSPACE", rsBACKSP }, { "TAB", rsTAB }, { "SPACE", ' ' }, { "UP", rsUP }, { "DOWN", rsDOWN }, { "LEFT", rsLEFT },
		{ "RIGHT", rsRIGHT }, { "HOME", rsHOME }, { "END", rsEND }, { "DEL", rsDEL }, { "INS", rsINS }, { "PGUP", rsPGUP },
		{ "PGDN", rsPGDN }, { "LSHIFT", rsLSHIFT }, { "RSHIFT", rsRSHIFT }, { "LCTRL", rsLCTRL }, { "RCTRL", rsRCTRL },
		{ "LALT", rsLALT }, { "RALT", rsRALT },
	};
	while (*s == ' ')
		s++;
	if (s[0] != '\0' && (s[1] == '\0' || s[1] == ' ')) // (one character: that key, a letter as its capital)
		return s[0] >= 'a' && s[0] <= 'z' ? s[0] - 'a' + 'A' : (uint8)s[0];
	for (int32 i = 0; i < (int32)ARRAY_SIZE(names); i++)
		if (SameWord(s, names[i].name))
			return names[i].code;
	if ((s[0] == 'F' || s[0] == 'f') && atoi(s + 1) >= 1 && atoi(s + 1) <= 12)
		return rsF1 + atoi(s + 1) - 1;
	return atoi(s); // (an rsKeyCodes number)
}

static bool
CoreCommand(const char *cmd, double now)
{
	if (strncmp(cmd, "key ", 4) == 0) {
		const int32 code = KeyCode(cmd + 4);
		if (code > 0 && code != rsNULL) {
			// (pressed as the keyboard does, held a few frames so that the pads see it)
			RsKeyCodes key = (RsKeyCodes)code;
			RsKeyboardEventHandler(rsKEYDOWN, &key);
			keyHeld = code;
			keyUpAt = now + 100.0;
			keyPumps = 0;
		}
		return true;
	}
	if (strcmp(cmd, "connect") == 0) {
		Connect();
		return true;
	}
	if (strcmp(cmd, "disconnect") == 0) {
		Disconnect();
		return true;
	}
	if (strcmp(cmd, "debug") == 0) {
		nextDebug = 0.0;
		return true;
	}
	return false;
}

static bool
CheatLikeCommand(const char *cmd)
{
	static const char *const words[] = { "tp", "weapon", "money", "car", "health", "armour", "give", "wanted" };
	for (const char *w : words) {
		const size_t k = strlen(w);
		if (strncmp(cmd, w, k) == 0 && (cmd[k] == ' ' || cmd[k] == '\0'))
			return true;
	}
	return false;
}

// (one command a frame; a key pressed by one is released before the next is read)
static void
TestStep(double now)
{
	if (keyHeld >= 0) {
		if (++keyPumps < 3 || now < keyUpAt)
			return;
		RsKeyCodes key = (RsKeyCodes)keyHeld;
		RsKeyboardEventHandler(rsKEYUP, &key);
		keyHeld = -1;
		gapPumps = 2;
		return;
	}
	if (gapPumps > 0) {
		gapPumps--;
		return;
	}
	char cmd[512];
	cmd[0] = '\0';
	if (!OrthrosNetTestCommand(cmd, sizeof(cmd)) || cmd[0] == '\0')
		return;
	debug("Orthros MMO: test command \"%s\"\n", cmd);
	if (CoreCommand(cmd, now))
		return;
	// (?mmotest is any page's to add: in a shared world, the commands that change the player's state are an
	// administrator's only, like the cheats)
	if (mode != MODE_OFF && mode != MODE_LOBBY && !admin && CheatLikeCommand(cmd)) {
		debug("Orthros MMO: test command \"%s\" refused (not an administrator)\n", cmd);
		return;
	}
	if (Session_TestCommand(cmd) || Lobby_TestCommand(cmd) || Players_TestCommand(cmd) || Entities_TestCommand(cmd) ||
	    Combat_TestCommand(cmd) || World_TestCommand(cmd) || Social_TestCommand(cmd))
		return;
	debug("Orthros MMO: no module took \"%s\"\n", cmd);
}

// ---------------------------------------------------------------- the debug state (?mmotest)
void
DebugAppend(const char *fmt, ...)
{
	const int32 room = (int32)sizeof(debugBuf) - debugLen;
	if (room <= 1) {
		debugOverflow = true;
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	const int32 k = vsnprintf(debugBuf + debugLen, room, fmt, ap);
	va_end(ap);
	if (k < 0)
		return;
	if (k >= room) {
		debugOverflow = true;
		debugLen = sizeof(debugBuf) - 1;
	} else
		debugLen += k;
}

// (a JSON string)
static void
DebugString(const char *s)
{
	char out[512];
	int32 n = 0;
	out[n++] = '"';
	for (const uint8 *c = (const uint8 *)s; *c && n < (int32)sizeof(out) - 8; c++) {
		if (*c == '"' || *c == '\\') {
			out[n++] = '\\';
			out[n++] = *c;
		} else if (*c < 0x20)
			n += sprintf(out + n, "\\u%04x", *c);
		else
			out[n++] = *c;
	}
	out[n++] = '"';
	out[n] = '\0';
	DebugAppend("%s", out);
}

static double
Finite(double v)
{
	return v == v && v < 1e300 && v > -1e300 ? v : 0.0;
}

static const char *
Bool(bool b)
{
	return b ? "true" : "false";
}

static void
PublishDebug(double now)
{
	nextDebug = now + 500.0;
	const double elapsed = now - lastDebug;
	const double fps = lastDebug > 0.0 && elapsed > 0.0 ? gameFrames * 1000.0 / elapsed : 0.0;
	lastDebug = now;
	gameFrames = 0;
	debugLen = 0;
	debugOverflow = false;

	char user[64];
	user[0] = '\0';
	OrthrosNetUser(user, sizeof(user));
	DebugAppend("{\"mode\":%d,\"link\":%d,\"refusal\":%d,\"slot\":%d,\"admin\":%s,\"serverOffset\":%.1f", mode, linkState,
	            refusal, localSlot == NO_SLOT ? -1 : localSlot, Bool(admin), Finite(serverOffset));
	DebugAppend(",\"active\":%s,\"entering\":%s,\"hello\":%s,\"tag\":%d,\"rank\":%u,\"rtt\":%.1f,\"clockSynced\":%s,\"fps\":%.1f",
	            Bool(IsActive()), Bool(IsEntering()), Bool(helloSent), localTag, localRank, Finite(numSamples ? bestRtt : -1.0),
	            Bool(haveOffset), Finite(fps));
	DebugAppend(",\"user\":");
	DebugString(user);
	DebugAppend(",\"refusalDetail\":");
	DebugString(refusalDetail);

	DebugAppend(",\"lobby\":{\"valid\":%s,\"serial\":%u,\"admin\":%s,\"maxCharacters\":%d,\"username\":", Bool(lobby.valid),
	            lobby.serial, Bool(lobby.admin), lobby.maxCharacters);
	DebugString(lobby.username);
	DebugAppend(",\"characters\":[");
	for (int32 i = 0; i < lobby.count; i++) {
		const LobbyCharacter &c = lobby.chars[i];
		DebugAppend("%s{\"id\":%u,\"name\":", i ? "," : "", c.id);
		DebugString(c.name);
		DebugAppend(",\"skin\":");
		DebugString(c.skin);
		DebugAppend(",\"money\":%d,\"online\":%s}", c.money, Bool(c.online));
	}
	DebugAppend("],\"result\":{\"op\":%d,\"status\":%d,\"id\":%u,\"serial\":%u}}", lobby.resultOp, lobby.resultStatus,
	            lobby.resultId, lobby.resultSerial);

	DebugAppend(",\"me\":{\"id\":%u,\"name\":", me.id);
	DebugString(me.name);
	DebugAppend(",\"skin\":");
	DebugString(me.skin);
	DebugAppend(",\"stateLength\":%d}", me.stateLength);

	DebugAppend(",\"world\":{\"valid\":%s,\"minutes\":%.2f,\"msPerMinute\":%.1f,\"serial\":%u,\"vars\":{", Bool(world.valid),
	            Finite(WorldMinutes()), Finite(world.msPerMinute), world.serial);
	for (int32 i = 0, k = 0; i < WV_COUNT; i++)
		if (world.varSet[i])
			DebugAppend("%s\"%d\":%d", k++ ? "," : "", i, world.vars[i]);
	DebugAppend("}}");

	CPlayerPed *ped = mode == MODE_ACTIVE ? FindPlayerPed() : nil;
	DebugAppend(",\"players\":[");
	for (int32 i = 0, k = 0; i < MAX_SLOTS; i++) {
		const RemotePlayer &p = players[i];
		if (!p.present)
			continue;
		const double dist = ped && p.hasState ? (p.pos - ped->GetPosition()).Magnitude() : -1.0;
		DebugAppend("%s{\"slot\":%d,\"name\":", k++ ? "," : "", p.slot);
		DebugString(p.name);
		DebugAppend(",\"skin\":");
		DebugString(p.skin);
		DebugAppend(",\"rank\":%u,\"admin\":%s,\"away\":%s,\"pos\":[%.2f,%.2f,%.2f],\"hasState\":%s,\"dist\":%.1f,\"area\":%d,"
		            "\"stars\":%d,\"puppet\":%s}",
		            p.rank, Bool(p.admin), Bool(p.away), Finite(p.pos.x), Finite(p.pos.y), Finite(p.pos.z), Bool(p.hasState),
		            Finite(dist), p.area, p.wanted, Bool(p.puppet != nil));
	}
	DebugAppend("]");

	int32 owned = 0, puppets = 0;
	for (int32 i = 0; i < RegistrySize(); i++)
		if (const NetEntity *ne = RegistryAt(i))
			ne->puppet ? puppets++ : owned++;
	DebugAppend(",\"registry\":{\"owned\":%d,\"puppets\":%d}", owned, puppets);

	// (each module's own keys; if they overflow the buffer, the core's alone)
	const int32 coreLen = debugLen;
	Session_Debug();
	Lobby_Debug();
	Players_Debug();
	Entities_Debug();
	Combat_Debug();
	World_Debug();
	Social_Debug();
	if (debugOverflow) {
		debugLen = coreLen;
		debugBuf[debugLen] = '\0';
		debugOverflow = false;
		DebugAppend(",\"debugOverflow\":true");
	}
	DebugAppend("}");
	OrthrosNetDebug(debugBuf);
}

// ---------------------------------------------------------------- the game's hooks (mmo.h)
void
Pump(void)
{
	if (!PageLink())
		return;
	const double now = Now();
	helloThisPump = false;
	if (testing)
		TestStep(now);
	if (!Dormant()) {
		PollLink();
		// (a link no longer wanted, closing: what it still brings is not dispatched)
		if (linkWanted)
			Receive();
		else
			DropPage();
		if (linkState == LINK_OPEN && helloSent) {
			if (now >= nextPing) {
				nextPing = now + 2000.0;
				Out o(C_PING);
				o.F64(now);
			}
			TrackAway();
		}
	}
	if (testing && now >= nextDebug)
		PublishDebug(now);
}

void
BeginProcess(void)
{
	if (Dormant())
		return;
	Session_BeginProcess();
	World_BeginProcess();
	Social_BeginProcess();
}

void
PreWorld(void)
{
	if (Dormant())
		return;
	Session_PreWorld();
	World_PreWorld();
	Players_PreWorld();
	Entities_PreWorld();
	Combat_PreWorld();
}

void
PostWorld(void)
{
	if (Dormant())
		return;
	Players_PostWorld();
	Entities_PostWorld();
	Combat_PostWorld();
	Session_PostWorld();
}

void
EndProcess(void)
{
	if (Dormant())
		return;
	gameFrames++;
	Session_EndProcess();
	Flush();
}

void
FrontendFrame(void)
{
	// (the lobby's start-menu row and its pages' switches are kept even while no shared world is wanted)
	if (Dormant()) {
		Lobby_FrontendFrame();
		return;
	}
	Session_FrontendFrame();
	Lobby_FrontendFrame();
	Flush();
}

void
Render2d(void)
{
	if (Dormant())
		return;
	Players_Render2d();
}

void
RenderOverlay(void)
{
	if (Dormant())
		return;
	Social_RenderOverlay();
}

bool
KeyDown(int32 rsKey)
{
	if (Dormant())
		return false;
	return Social_KeyDown(rsKey) || Lobby_KeyDown(rsKey);
}

void
TextInput(const char *utf8)
{
	if (Dormant())
		return;
	Social_TextInput(utf8);
	Lobby_TextInput(utf8);
}

bool
OverlayHasMouse(void)
{
	if (Dormant())
		return false;
	return Social_OverlayHasMouse();
}

void
OverlayMouse(float x, float y)
{
	if (Dormant())
		return;
	Social_OverlayMouse(x, y);
}

bool
OverlayClick(float x, float y)
{
	if (Dormant() || !Social_OverlayClick(x, y))
		return false;
	OrthrosExtras::SwallowLeftClick(); // (the click is the list's, not the game's, until it is released)
	return true;
}

void
LocalPlayerDied(void)
{
	if (Dormant())
		return;
	Session_PlayerDied();
	Combat_PlayerDied();
}

void
LocalPlayerRespawned(void)
{
	if (Dormant())
		return;
	Session_PlayerRespawned();
	Combat_PlayerRespawned();
}
}
#endif
