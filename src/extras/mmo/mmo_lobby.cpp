// The shared world's lobby: the main menu's "Multiplayer" entry and its pages (docs/mmo.md, section 7 "lobby" and
// "Session flow"). The characters list, a character (Play, Appearance, Delete), a new character (its pseudo typed in a
// text field, its appearance), the delete confirmation; the link's state on the list page (connecting, sign-in needed,
// unavailable, full...). The pages are the game's own (static entries of aScreens: MENUPAGE_MMO_*), their rows written
// here at run time, their texts "ORT_*" keys answered here (Lobby_Text, through OrthrosExtras::Text).
// The appearance (the character's and the new character's): chosen on the appearance picker's page, MENUPAGE_MMO_SKINS,
// opened from the Appearance row (mmo_skins.cpp: the skins' list, the picker and its preview).
#include "common.h"

#ifdef ORTHROS_EXTRAS
#include "mmo_internal.h"
#include "Frontend.h"
#include "Text.h"
#include "Pad.h"
#include "rwcore.h"
#include "skeleton.h"

namespace OrthrosMmo
{
// ---------------------------------------------------------------- texts
// (a UTF-8 text in the game's codes, into a buffer of its own)
static wchar *
Game(const char *utf8, wchar *out, int32 size)
{
	GameText(utf8, out, size);
	return out;
}

static const char *
Tr(const char *en, const char *fr)
{
	return LangFrench() ? fr : en;
}

// (a skin's model name kept: the list's spelling, "" for none)
static void
SetSkin(char *out, const char *model)
{
	const char *listed = model ? Lobby_SkinModel(model) : nil;
	snprintf(out, 24, "%s", listed ? listed : model ? model : "");
}

// ---------------------------------------------------------------- the lobby's state
enum {
	PAGE_FIRST = MENUPAGE_MMO_CHARACTERS,
	NUM_PAGES = MENUPAGE_MMO_SKINS - MENUPAGE_MMO_CHARACTERS + 1,
	MAX_LIST_CHARS = sizeof(LobbyData::chars) / sizeof(LobbyData::chars[0]),
	MAX_VISIBLE = 12,             // (the frontend's MAX_VISIBLE_OPTION: more rows scroll)
	NAME_MAX = 16,
	CONNECT_TIMEOUT_MS = 10000,
	SIGN_IN_RETRY_MS = 4000, SIGN_IN_RETRY_FOR_MS = 180000,
};

// The list page's state: the link's, the lobby's.
enum { ST_LIST, ST_CONNECTING, ST_NO_SERVER, ST_SIGN_IN, ST_UNAVAILABLE, ST_OUTDATED, ST_FULL, ST_ELSEWHERE, ST_ERROR };
static const char *statusNames[] = { "list", "connecting", "no server", "sign in", "unavailable", "outdated", "full", "elsewhere", "error" };

// A message shown in a page's label until the next action (CHAR_RESULT, a refused press).
enum { M_NONE, M_INVALID_NAME, M_NAME_TAKEN, M_TOO_MANY, M_NOT_FOUND, M_WORLD_FULL, M_ERROR, M_INVALID_SKIN, M_SKIN_SAVED,
	M_DELETED, M_LOAD_FAILED };
static const char *messageNames[] = { "", "invalid name", "name taken", "too many", "not found", "world full", "error",
	"invalid skin", "skin saved", "deleted", "load failed" };

// The rows of our pages, by role (the press callbacks read the pressed row's).
enum { R_NONE, R_LABEL, R_CHARACTER, R_NEW, R_BACK, R_SIGN_IN, R_RETRY, R_PLAY, R_APPEARANCE, R_DELETE, R_NAME, R_CREATE,
	R_NO, R_YES };
struct Row { int8 role, index; };

static struct {
	Row rows[NUM_PAGES][NUM_MENUROWS];
	int32 numRows[NUM_PAGES];
	uint32 shape[NUM_PAGES];          // (what the list page's rows were built for: rebuilt when it changes)
	CCFODynamic cfo[NUM_PAGES][NUM_MENUROWS];
	CCustomScreenLayout layout[NUM_PAGES];
	bool entered;                     // (our pages are open: the link asked for)
	double connectStart, signInAt, lastRetry;
	bool quietRetry;                  // (retrying after a sign-in: its state kept on screen meanwhile)
	int32 shownStatus;
	uint32 seenLobby, seenResult;
	int32 message, messagePage;
	uint32 selectedId;                // (the character page's)
	char skinChoice[24];              // (its appearance as shown: the stored one, or the one just chosen until stored)
	char newName[NAME_MAX + 1], nameBefore[NAME_MAX + 1];
	char newSkin[24];
	bool editing, creating, deleting, joining;
	uint32 pendingOpenId;             // (a character just created: its page once the lobby lists it)
	int32 pendingScreen;              // (a page to switch to at the next frontend frame)
	int32 editDummy;                  // (FrontEndMenuManager.pEditString while typing: the menu's navigation frozen)
	int32 pickerDummy;                // (the same while the appearance picker is shown: its keys and mouse are its own)
	int32 pickerFor;                  // (the page the picker was opened from, and goes back to)
	CMenuScreenCustom::CMenuEntry startRow; bool startRowSaved; int32 startRowAt;
} lobby;

static bool
OurPage(int32 screen)
{
	return screen >= MENUPAGE_MMO_CHARACTERS && screen <= MENUPAGE_MMO_SKINS;
}

static const LobbyCharacter *
FindCharacter(uint32 id, int32 *index = nil)
{
	const LobbyData &l = Lobby();
	for (int32 i = 0; i < l.count && i < MAX_LIST_CHARS; i++)
		if (l.chars[i].id == id) {
			if (index)
				*index = i;
			return &l.chars[i];
		}
	return nil;
}

static const LobbyCharacter *
FindCharacterNamed(const char *name)
{
	const LobbyData &l = Lobby();
	for (int32 i = 0; i < l.count && i < MAX_LIST_CHARS; i++)
		if (strcasecmp(l.chars[i].name, name) == 0)
			return &l.chars[i];
	return nil;
}

// (the name rules of the server: 2 to 16 ASCII characters, letters, digits, space, _ . -, a letter or a digit first)
static bool
NameCharOk(char c, bool first)
{
	if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
		return true;
	return !first && (c == ' ' || c == '_' || c == '.' || c == '-');
}

static bool
ValidName(const char *name)
{
	const int32 n = (int32)strlen(name);
	if (n < 2 || n > NAME_MAX)
		return false;
	for (int32 i = 0; i < n; i++) {
		if (!NameCharOk(name[i], i == 0))
			return false;
		// (as the server's rule: a separator only between two letters or digits)
		if (!NameCharOk(name[i], true) && (i + 1 >= n || !NameCharOk(name[i + 1], true)))
			return false;
	}
	return true;
}

// ---------------------------------------------------------------- the link's state
static int32
LinkStatus(void)
{
	if (!NetAvailable())
		return ST_UNAVAILABLE;
	switch (LinkState()) {
	case LINK_OPEN:
		return Lobby().valid ? ST_LIST : ST_CONNECTING;
	case LINK_REFUSED:
		switch (Refusal()) {
		case 1: return ST_SIGN_IN;
		case 2: return ST_UNAVAILABLE;
		case 3: return ST_FULL;
		case 4: return ST_OUTDATED;
		case 5: return ST_ELSEWHERE;
		default: return ST_ERROR;
		}
	default:
		return ST_CONNECTING;
	}
}

static int32
ListStatus(void)
{
	int32 s = LinkStatus();
	if (s == ST_CONNECTING) {
		if (lobby.quietRetry && lobby.shownStatus == ST_SIGN_IN)
			s = ST_SIGN_IN;
		else if (Now() - lobby.connectStart > CONNECT_TIMEOUT_MS)
			s = ST_NO_SERVER;
	} else if (s != ST_SIGN_IN)
		lobby.quietRetry = false;
	lobby.shownStatus = s;
	return s;
}

static void ClearPending(bool told);

static void
StartConnecting(bool quiet)
{
	// (a new link: nothing asked on the last one is answered on it)
	ClearPending(false);
	lobby.quietRetry = quiet;
	if (!quiet)
		lobby.connectStart = Now();
	lobby.lastRetry = Now();
	Connect();
}

static void
SetMessage(int32 message, int32 page)
{
	lobby.message = message;
	lobby.messagePage = page;
}

static int32
MessageOn(int32 page)
{
	return lobby.messagePage == page ? lobby.message : M_NONE;
}

// ---------------------------------------------------------------- the pseudo's text field
static void
StartEditing(void)
{
	if (lobby.editing)
		return;
	strcpy(lobby.nameBefore, lobby.newName);
	lobby.editing = true;
	FrontEndMenuManager.pEditString = &lobby.editDummy;
	SetMessage(M_NONE, -1);
}

static void
StopEditing(bool keep)
{
	if (!lobby.editing)
		return;
	lobby.editing = false;
	if (!keep)
		strcpy(lobby.newName, lobby.nameBefore);
	// (trailing spaces are no part of a name)
	for (int32 n = (int32)strlen(lobby.newName); n > 0 && lobby.newName[n - 1] == ' '; n--)
		lobby.newName[n - 1] = '\0';
	if (FrontEndMenuManager.pEditString == &lobby.editDummy)
		FrontEndMenuManager.pEditString = nil;
}

bool
Lobby_KeyDown(int32 rsKey)
{
	// (the appearance picker's keys: all of them while it is shown)
	if (FrontEndMenuManager.m_bMenuActive && FrontEndMenuManager.m_nCurrScreen == MENUPAGE_MMO_SKINS)
		return Skins_PickerKey(rsKey);
	if (!lobby.editing)
		return false;
	switch (rsKey) {
	case rsBACKSP: {
		const int32 n = (int32)strlen(lobby.newName);
		if (n > 0)
			lobby.newName[n - 1] = '\0';
		break;
	}
	case rsENTER:
	case rsPADENTER:
		StopEditing(true);
		break;
	case rsESC:
		StopEditing(false);
		break;
	}
	// (every key the field's while it is open: neither the menu nor the cheats see them)
	return true;
}

void
Lobby_TextInput(const char *utf8)
{
	if (!lobby.editing)
		return;
	for (const char *c = utf8; *c; c++) {
		int32 n = (int32)strlen(lobby.newName);
		if (n >= NAME_MAX)
			break;
		if (!NameCharOk(*c, n == 0))
			continue;
		lobby.newName[n] = *c;
		lobby.newName[n + 1] = '\0';
	}
}

// ---------------------------------------------------------------- requests
static void
ApplySkinChoice(void)
{
	const LobbyCharacter *c = FindCharacter(lobby.selectedId);
	if (c == nil || lobby.skinChoice[0] == '\0' || strcasecmp(c->skin, lobby.skinChoice) == 0)
		return;
	RequestSkin(c->id, lobby.skinChoice);
}

static void
LobbyPlay(uint32 id)
{
	if (lobby.joining || FindCharacter(id) == nil)
		return;
	ApplySkinChoice();
	SetMessage(M_NONE, -1);
	lobby.joining = true;
	RequestJoin(id);
}

static bool
LobbyCreate(const char *name, const char *skin)
{
	if (lobby.creating)
		return false;
	const LobbyData &l = Lobby();
	if (l.count >= l.maxCharacters) {
		SetMessage(M_TOO_MANY, MENUPAGE_MMO_NEW);
		return false;
	}
	if (!ValidName(name)) {
		SetMessage(M_INVALID_NAME, MENUPAGE_MMO_NEW);
		return false;
	}
	if (FindCharacterNamed(name)) {
		SetMessage(M_NAME_TAKEN, MENUPAGE_MMO_NEW);
		return false;
	}
	SetMessage(M_NONE, -1);
	lobby.creating = true;
	RequestCreate(name, skin);
	return true;
}

static void
LobbyDelete(uint32 id)
{
	SetMessage(M_NONE, -1);
	lobby.deleting = true;
	RequestDelete(id);
}

// (the requests waiting for an answer given up: the link that carried them is gone - closed, refused, reset -; said on
// the page shown if it stays, on the list otherwise)
static void
ClearPending(bool told)
{
	const bool pending = lobby.joining || lobby.creating || lobby.deleting;
	lobby.joining = lobby.creating = lobby.deleting = false;
	lobby.pendingOpenId = 0;
	if (pending && told) {
		const int32 screen = FrontEndMenuManager.m_nCurrScreen;
		SetMessage(M_ERROR, OurPage(screen) && LinkStatus() == ST_LIST ? screen : MENUPAGE_MMO_CHARACTERS);
	}
}

// (the join answered - WELCOME: the session's from then on -, or the requests' link no longer open)
static void
SettlePending(void)
{
	if (lobby.joining && Mode() != MODE_LOBBY && Mode() != MODE_OFF)
		lobby.joining = false;
	else if (LinkState() != LINK_OPEN || !Lobby().valid)
		ClearPending(true);
}

void
Lobby_LinkReset(void)
{
	if (Mode() == MODE_LOBBY || Mode() == MODE_OFF)
		ClearPending(true);
}

// (CHAR_RESULT and LOBBY records, as the core keeps them: acted upon once each)
static void
PollLobby(void)
{
	const LobbyData &l = Lobby();
	if (l.resultSerial != lobby.seenResult) {
		lobby.seenResult = l.resultSerial;
		const int32 status = l.resultStatus;
		static const int32 errors[] = { M_NONE, M_INVALID_NAME, M_NAME_TAKEN, M_TOO_MANY, M_NOT_FOUND, M_WORLD_FULL, M_ERROR, M_INVALID_SKIN };
		const int32 error = status >= 0 && status < (int32)ARRAY_SIZE(errors) ? errors[status] : M_ERROR;
		switch (l.resultOp) {
		case 2: // create
			lobby.creating = false;
			if (status == 0) {
				lobby.pendingOpenId = l.resultId;
				lobby.newName[0] = '\0';
			} else
				SetMessage(error, MENUPAGE_MMO_NEW);
			break;
		case 3: // skin
			SetMessage(status == 0 ? M_SKIN_SAVED : error, MENUPAGE_MMO_CHARACTER);
			break;
		case 4: // delete
			lobby.deleting = false;
			SetMessage(status == 0 ? M_DELETED : error, MENUPAGE_MMO_CHARACTERS);
			break;
		case 5: // join (only its failures: WELCOME is the session's)
			if (status != 0) {
				lobby.joining = false;
				SetMessage(error, MENUPAGE_MMO_CHARACTER);
			}
			break;
		}
	}
	if (l.serial != lobby.seenLobby) {
		lobby.seenLobby = l.serial;
		const int32 screen = FrontEndMenuManager.m_nCurrScreen;
		int32 index;
		if (lobby.pendingOpenId && FindCharacter(lobby.pendingOpenId, &index)) {
			lobby.selectedId = lobby.pendingOpenId;
			SetSkin(lobby.skinChoice, l.chars[index].skin);
			lobby.pendingOpenId = 0;
			if (screen == MENUPAGE_MMO_NEW)
				lobby.pendingScreen = MENUPAGE_MMO_CHARACTER;
		}
		const LobbyCharacter *c = FindCharacter(lobby.selectedId);
		if (c == nil && (screen == MENUPAGE_MMO_CHARACTER || screen == MENUPAGE_MMO_DELETE ||
		                 (screen == MENUPAGE_MMO_SKINS && lobby.pickerFor == MENUPAGE_MMO_CHARACTER)))
			lobby.pendingScreen = MENUPAGE_MMO_CHARACTERS;
		// (the appearance shown is the stored one: a choice sent is shown until the lobby lists it)
		if (c)
			SetSkin(lobby.skinChoice, c->skin);
	}
}

// ---------------------------------------------------------------- the rows
static void SelectRow(int32 page, int32 role, int32 index);

static Row &
PressedRow(void)
{
	static Row none;
	const int32 screen = FrontEndMenuManager.m_nCurrScreen, option = FrontEndMenuManager.m_nCurrOption;
	if (!OurPage(screen) || option < 0 || option >= NUM_MENUROWS)
		return none;
	return lobby.rows[screen - PAGE_FIRST][option];
}

// (one draw function per character row: the frontend tells it nothing of its row)
static wchar *
CharacterRight(int32 i, bool *disabled)
{
	static wchar buffers[MAX_LIST_CHARS][48];
	const LobbyData &l = Lobby();
	if (i >= l.count)
		return nil;
	char utf8[48];
	if (l.chars[i].online)
		snprintf(utf8, sizeof(utf8), "%s  $%d", Tr("(in game)", "(en jeu)"), l.chars[i].money);
	else
		snprintf(utf8, sizeof(utf8), "$%d", l.chars[i].money);
	return Game(utf8, buffers[i], ARRAY_SIZE(buffers[i]));
}
#define CHARACTER_DRAW(i) static wchar *CharacterDraw##i(bool *disabled, bool) { return CharacterRight(i, disabled); }
CHARACTER_DRAW(0) CHARACTER_DRAW(1) CHARACTER_DRAW(2) CHARACTER_DRAW(3) CHARACTER_DRAW(4) CHARACTER_DRAW(5)
CHARACTER_DRAW(6) CHARACTER_DRAW(7) CHARACTER_DRAW(8) CHARACTER_DRAW(9) CHARACTER_DRAW(10) CHARACTER_DRAW(11)
CHARACTER_DRAW(12) CHARACTER_DRAW(13) CHARACTER_DRAW(14) CHARACTER_DRAW(15)
#undef CHARACTER_DRAW
static const DrawFunc characterDraws[MAX_LIST_CHARS] = {
	CharacterDraw0, CharacterDraw1, CharacterDraw2, CharacterDraw3, CharacterDraw4, CharacterDraw5, CharacterDraw6,
	CharacterDraw7, CharacterDraw8, CharacterDraw9, CharacterDraw10, CharacterDraw11, CharacterDraw12, CharacterDraw13,
	CharacterDraw14, CharacterDraw15,
};

static void
CharacterPress(int8 action)
{
	if (action != FEOPTION_ACTION_SELECT)
		return;
	const LobbyData &l = Lobby();
	const int32 i = PressedRow().index;
	if (i < 0 || i >= l.count)
		return;
	lobby.selectedId = l.chars[i].id;
	SetSkin(lobby.skinChoice, l.chars[i].skin);
	SetMessage(M_NONE, -1);
	FrontEndMenuManager.SwitchToNewScreen(MENUPAGE_MMO_CHARACTER);
}

static wchar *
NewDraw(bool *disabled, bool)
{
	*disabled = Lobby().count >= Lobby().maxCharacters;
	return nil;
}

static void
NewPress(int8 action)
{
	if (action != FEOPTION_ACTION_SELECT)
		return;
	if (Lobby().count >= Lobby().maxCharacters) {
		SetMessage(M_TOO_MANY, MENUPAGE_MMO_CHARACTERS);
		return;
	}
	SetMessage(M_NONE, -1);
	FrontEndMenuManager.SwitchToNewScreen(MENUPAGE_MMO_NEW);
}

static void
SignInPress(int8 action)
{
	if (action != FEOPTION_ACTION_SELECT)
		return;
	SignIn();
	lobby.signInAt = Now();
}

static void
RetryPress(int8 action)
{
	if (action != FEOPTION_ACTION_SELECT)
		return;
	lobby.signInAt = 0.0;
	StartConnecting(false);
}

static wchar *
PlayDraw(bool *disabled, bool)
{
	*disabled = lobby.joining;
	return nil;
}

static void
PlayPress(int8 action)
{
	if (action == FEOPTION_ACTION_SELECT)
		LobbyPlay(lobby.selectedId);
}

static wchar *
AppearanceDraw(bool *disabled, bool)
{
	static wchar buffer[64];
	if (lobby.skinChoice[0])
		return Skins_Label(lobby.skinChoice, buffer, ARRAY_SIZE(buffer));
	const LobbyCharacter *c = FindCharacter(lobby.selectedId);
	return c ? Skins_Label(c->skin, buffer, ARRAY_SIZE(buffer)) : nil;
}

// (the appearance picker opened from a page's Appearance row, Enter or a click; it comes back with its choice)
static void
OpenPicker(int32 page, const char *current)
{
	lobby.pickerFor = page;
	Skins_PickerOpen(current);
	FrontEndMenuManager.SwitchToNewScreen(MENUPAGE_MMO_SKINS);
}

static void
AppearancePress(int8 action)
{
	if (action != FEOPTION_ACTION_SELECT)
		return;
	if (lobby.messagePage == MENUPAGE_MMO_CHARACTER)
		SetMessage(M_NONE, -1);
	if (lobby.joining)
		return;
	const LobbyCharacter *c = FindCharacter(lobby.selectedId);
	OpenPicker(MENUPAGE_MMO_CHARACTER, lobby.skinChoice[0] ? lobby.skinChoice : c ? c->skin : nil);
}

// (the picker closed: its choice (sent at once for a character, kept for a new one) and back to its page)
void
Lobby_SkinPicked(const char *model)
{
	CMenuManager &m = FrontEndMenuManager;
	const int32 back = lobby.pickerFor == MENUPAGE_MMO_NEW ? MENUPAGE_MMO_NEW : MENUPAGE_MMO_CHARACTER;
	if (model && back == MENUPAGE_MMO_NEW)
		SetSkin(lobby.newSkin, model);
	else if (model) {
		const LobbyCharacter *c = FindCharacter(lobby.selectedId);
		if (c && strcasecmp(c->skin, model) != 0) {
			SetSkin(lobby.skinChoice, model);
			ApplySkinChoice();
		}
	}
	if (m.m_nCurrScreen == MENUPAGE_MMO_SKINS)
		m.SwitchToNewScreen(back);
}

static void
DeletePress(int8 action)
{
	if (action == FEOPTION_ACTION_SELECT)
		FrontEndMenuManager.SwitchToNewScreen(MENUPAGE_MMO_DELETE);
}

static void
YesPress(int8 action)
{
	if (action != FEOPTION_ACTION_SELECT)
		return;
	if (FindCharacter(lobby.selectedId))
		LobbyDelete(lobby.selectedId);
	FrontEndMenuManager.SwitchToNewScreen(MENUPAGE_MMO_CHARACTERS);
}

static wchar *
NameDraw(bool *disabled, bool)
{
	static wchar buffer[NAME_MAX + 4];
	char utf8[NAME_MAX + 4];
	// (the caret while typing: the heading font has '_' from the standard one)
	snprintf(utf8, sizeof(utf8), "%s%s", lobby.newName, lobby.editing ? "_" : "");
	return utf8[0] ? Game(utf8, buffer, ARRAY_SIZE(buffer)) : nil;
}

static void
NamePress(int8 action)
{
	if (action == FEOPTION_ACTION_SELECT)
		StartEditing();
}

static wchar *
NewAppearanceDraw(bool *disabled, bool)
{
	static wchar buffer[64];
	if (lobby.newSkin[0] == '\0')
		SetSkin(lobby.newSkin, "player");
	return Skins_Label(lobby.newSkin, buffer, ARRAY_SIZE(buffer));
}

static void
NewAppearancePress(int8 action)
{
	if (action != FEOPTION_ACTION_SELECT)
		return;
	if (lobby.editing)
		StopEditing(true);
	if (lobby.newSkin[0] == '\0')
		SetSkin(lobby.newSkin, "player");
	OpenPicker(MENUPAGE_MMO_NEW, lobby.newSkin);
}

static wchar *
CreateDraw(bool *disabled, bool)
{
	*disabled = lobby.creating || !ValidName(lobby.newName);
	return nil;
}

static void
CreatePress(int8 action)
{
	if (action == FEOPTION_ACTION_SELECT)
		LobbyCreate(lobby.newName, lobby.newSkin[0] ? lobby.newSkin : "player");
}

// (a page's rows written: each its role, key, action, place; the rest of the page cleared)
static int32 rowCount;

static void
AddRow(int32 page, int32 role, int32 index, int32 action, const char *key, DrawFunc draw, ButtonPressFunc press,
	uint16 x, uint16 y, uint8 align, int32 target = MENUPAGE_NONE)
{
	const int32 p = page - PAGE_FIRST;
	if (rowCount >= NUM_MENUROWS)
		return;
	CMenuScreenCustom::CMenuEntry &e = aScreens[page].m_aEntries[rowCount];
	memset(&e, 0, sizeof(e));
	e.m_Action = action;
	strncpy(e.m_EntryName, key, sizeof(e.m_EntryName) - 1);
	if (action == MENUACTION_CFO_DYNAMIC) {
		CCFODynamic &cfo = lobby.cfo[p][rowCount];
		cfo.value = nil;
		cfo.saveCat = nil;
		cfo.save = nil;
		cfo.drawFunc = draw;
		cfo.buttonPressFunc = press;
		e.m_CFODynamic = &cfo;
	}
	e.m_SaveSlot = SAVESLOT_NONE;
	e.m_TargetMenu = target;
	e.m_X = x;
	e.m_Y = y;
	e.m_Align = align;
	lobby.rows[p][rowCount].role = role;
	lobby.rows[p][rowCount].index = index;
	rowCount++;
}

static void
EndRows(int32 page, int32 lineHeight, bool wide)
{
	const int32 p = page - PAGE_FIRST;
	for (int32 i = rowCount; i < NUM_MENUROWS; i++) {
		memset(&aScreens[page].m_aEntries[i], 0, sizeof(aScreens[page].m_aEntries[i]));
		lobby.rows[p][i].role = R_NONE;
		lobby.rows[p][i].index = -1;
	}
	lobby.numRows[p] = rowCount;
	CCustomScreenLayout &layout = lobby.layout[p];
	layout.startX = 320;
	layout.startY = 160;
	layout.lineHeight = lineHeight; // (the mouse's band of a row: as far as the next)
	layout.noInvasiveBorders = wide;
	layout.xMargin = 0;
	aScreens[page].layout = &layout;
}

static uint32
ListShape(int32 status)
{
	const LobbyData &l = Lobby();
	return status == ST_LIST ? 0x10000u | (uint32)Min(l.count, (int32)MAX_LIST_CHARS) : 0x20000u | (uint32)status;
}

static void
BuildList(int32 status)
{
	const int32 page = MENUPAGE_MMO_CHARACTERS;
	rowCount = 0;
	if (status == ST_LIST) {
		const LobbyData &l = Lobby();
		const int32 count = Min(l.count, (int32)MAX_LIST_CHARS);
		// (the label — the account, messages — unless the rows would scroll: more than the frontend shows at once)
		const int32 options = count + 2;
		const bool withLabel = options + 1 <= MAX_VISIBLE;
		const int32 line = options <= 8 ? 28 : 22;
		int32 y = withLabel ? 160 : 100;
		if (withLabel)
			AddRow(page, R_LABEL, -1, MENUACTION_LABEL, "ORT_STA", nil, nil, 0, 0, 0);
		for (int32 i = 0; i < count; i++, y += line) {
			char key[8];
			snprintf(key, sizeof(key), "ORT_C%d", i + 1);
			AddRow(page, R_CHARACTER, i, MENUACTION_CFO_DYNAMIC, key, characterDraws[i], CharacterPress, 40, y, MENUALIGN_LEFT);
		}
		AddRow(page, R_NEW, -1, MENUACTION_CFO_DYNAMIC, "ORT_NCH", NewDraw, NewPress, 320, y, MENUALIGN_CENTER);
		y += line;
		AddRow(page, R_BACK, -1, MENUACTION_GOBACK, "FEDS_TB", nil, nil, 320, y, MENUALIGN_CENTER);
		EndRows(page, line, true);
	} else {
		int32 y = 175;
		AddRow(page, R_LABEL, -1, MENUACTION_LABEL, "ORT_STA", nil, nil, 0, 0, 0);
		if (status == ST_SIGN_IN) {
			AddRow(page, R_SIGN_IN, -1, MENUACTION_CFO_DYNAMIC, "ORT_SIN", nil, SignInPress, 320, y, MENUALIGN_CENTER);
			y += 28;
		}
		if (status == ST_SIGN_IN || status == ST_NO_SERVER || status == ST_FULL || status == ST_ELSEWHERE || status == ST_ERROR) {
			AddRow(page, R_RETRY, -1, MENUACTION_CFO_DYNAMIC, "ORT_RTY", nil, RetryPress, 320, y, MENUALIGN_CENTER);
			y += 28;
		}
		AddRow(page, R_BACK, -1, MENUACTION_GOBACK, "FEDS_TB", nil, nil, 320, y + 14, MENUALIGN_CENTER);
		EndRows(page, 28, false);
	}
	lobby.shape[page - PAGE_FIRST] = ListShape(status);
}

static void
BuildCharacter(void)
{
	const int32 page = MENUPAGE_MMO_CHARACTER;
	rowCount = 0;
	AddRow(page, R_LABEL, -1, MENUACTION_LABEL, "ORT_STA", nil, nil, 0, 0, 0);
	AddRow(page, R_PLAY, -1, MENUACTION_CFO_DYNAMIC, "ORT_PLY", PlayDraw, PlayPress, 40, 160, MENUALIGN_LEFT);
	AddRow(page, R_APPEARANCE, -1, MENUACTION_CFO_DYNAMIC, "ORT_APP", AppearanceDraw, AppearancePress, 40, 188, MENUALIGN_LEFT);
	AddRow(page, R_DELETE, -1, MENUACTION_CFO_DYNAMIC, "ORT_DEL", nil, DeletePress, 40, 216, MENUALIGN_LEFT);
	AddRow(page, R_BACK, -1, MENUACTION_GOBACK, "FEDS_TB", nil, nil, 320, 260, MENUALIGN_CENTER);
	EndRows(page, 28, true);
}

static void
BuildNew(void)
{
	const int32 page = MENUPAGE_MMO_NEW;
	rowCount = 0;
	AddRow(page, R_LABEL, -1, MENUACTION_LABEL, "ORT_STA", nil, nil, 0, 0, 0);
	AddRow(page, R_NAME, -1, MENUACTION_CFO_DYNAMIC, "ORT_NAM", NameDraw, NamePress, 40, 160, MENUALIGN_LEFT);
	AddRow(page, R_APPEARANCE, -1, MENUACTION_CFO_DYNAMIC, "ORT_APP", NewAppearanceDraw, NewAppearancePress, 40, 188, MENUALIGN_LEFT);
	AddRow(page, R_CREATE, -1, MENUACTION_CFO_DYNAMIC, "ORT_CRE", CreateDraw, CreatePress, 320, 232, MENUALIGN_CENTER);
	AddRow(page, R_BACK, -1, MENUACTION_GOBACK, "FEDS_TB", nil, nil, 320, 260, MENUALIGN_CENTER);
	EndRows(page, 28, true);
}

static void
BuildDelete(void)
{
	const int32 page = MENUPAGE_MMO_DELETE;
	rowCount = 0;
	// (as the game's own confirmations: its question, No, Yes)
	AddRow(page, R_LABEL, -1, MENUACTION_LABEL, "ORT_DLQ", nil, nil, 0, 0, 0);
	AddRow(page, R_NO, -1, MENUACTION_NO, "FEM_NO", nil, nil, 320, 200, MENUALIGN_CENTER, MENUPAGE_MMO_CHARACTER);
	AddRow(page, R_YES, -1, MENUACTION_CFO_DYNAMIC, "FEM_YES", nil, YesPress, 320, 225, MENUALIGN_CENTER);
	EndRows(page, 25, false);
}

// (the appearance picker's page: no rows, the picker draws and drives it; the menus' wide frame)
static void
BuildSkins(void)
{
	rowCount = 0;
	EndRows(MENUPAGE_MMO_SKINS, 28, true);
}

static int32
FindRow(int32 page, int32 role, int32 index)
{
	const int32 p = page - PAGE_FIRST;
	for (int32 i = 0; i < lobby.numRows[p]; i++)
		if (lobby.rows[p][i].role == role && (index < 0 || lobby.rows[p][i].index == index))
			return i;
	return -1;
}

static int32
FirstOption(int32 page)
{
	const int32 p = page - PAGE_FIRST;
	for (int32 i = 0; i < lobby.numRows[p]; i++)
		if (lobby.rows[p][i].role != R_LABEL)
			return i;
	return 0;
}

// (the cursor on a row of the page shown, the frontend's scrolling set for the rows it has now — it counts them only
// when a page opens)
static void
SelectRow(int32 page, int32 role, int32 index)
{
	CMenuManager &m = FrontEndMenuManager;
	if (m.m_nCurrScreen != page)
		return;
	int32 row = FindRow(page, role, index);
	if (row < 0)
		row = FirstOption(page);
	m.m_nCurrOption = row;
	m.m_nOptionHighlightTransitionBlend = 0;
	const int32 total = lobby.numRows[page - PAGE_FIRST];
	m.m_nTotalListRow = total;
	if (total > MAX_VISIBLE) {
		const int32 first = Clamp(row - (MAX_VISIBLE - 1), 0, total - MAX_VISIBLE);
		m.m_nSelectedListRow = row;
		m.m_nFirstVisibleRowOnList = first;
		m.m_nScrollbarTopMargin = (263.0f / total) * first; // (the frontend's SCROLLBAR_MAX_HEIGHT)
	} else {
		m.m_nSelectedListRow = 0;
		m.m_nFirstVisibleRowOnList = 0;
		m.m_nScrollbarTopMargin = 0.0f;
	}
}

// (the list page rebuilt when what it shows changed — the link's state, the number of characters —, the cursor kept on
// the same thing)
static void
RefreshList(void)
{
	const int32 page = MENUPAGE_MMO_CHARACTERS;
	const int32 status = ListStatus();
	if (lobby.shape[page - PAGE_FIRST] == ListShape(status) && lobby.numRows[page - PAGE_FIRST] > 0)
		return;
	CMenuManager &m = FrontEndMenuManager;
	Row was = { R_NONE, -1 };
	if (m.m_nCurrScreen == page && m.m_nCurrOption >= 0 && m.m_nCurrOption < NUM_MENUROWS)
		was = lobby.rows[page - PAGE_FIRST][m.m_nCurrOption];
	// (what the page says changed - the list come, a sign-in asked...: the cursor on what it offers first; the list's
	// own changes keep it where it was)
	const bool changed = !((lobby.shape[page - PAGE_FIRST] & 0x10000u) && status == ST_LIST);
	BuildList(status);
	if (m.m_nCurrScreen == page) {
		if (changed)
			was = status != ST_LIST ? Row{ R_NONE, -1 } : Lobby().count > 0 ? Row{ R_CHARACTER, 0 } : Row{ R_NEW, -1 };
		if (was.role == R_CHARACTER && was.index >= Lobby().count)
			was.index = Lobby().count - 1;
		SelectRow(page, was.role, was.role == R_CHARACTER ? was.index : -1);
	}
}

// ---------------------------------------------------------------- the main menu's entry
// ("Multiplayer" only where the page offers a shared world: the row out of the start page otherwise, back if it comes)
static void
SyncStartRow(void)
{
	CMenuScreenCustom::CMenuEntry *e = aScreens[MENUPAGE_START_MENU].m_aEntries;
	int32 count = 0, at = -1;
	for (; count < NUM_MENUROWS && e[count].m_Action != MENUACTION_NOTHING; count++)
		if (e[count].m_Action == MENUACTION_CHANGEMENU && e[count].m_TargetMenu == MENUPAGE_MMO_CHARACTERS)
			at = count;
	const bool want = NetAvailable();
	if (want == (at >= 0))
		return;
	if (!want) {
		lobby.startRow = e[at];
		lobby.startRowSaved = true;
		lobby.startRowAt = at;
		memmove(&e[at], &e[at + 1], (count - at - 1) * sizeof(e[0]));
		memset(&e[count - 1], 0, sizeof(e[0]));
		count--;
	} else {
		if (!lobby.startRowSaved || count >= NUM_MENUROWS || lobby.startRowAt > count)
			return;
		memmove(&e[lobby.startRowAt + 1], &e[lobby.startRowAt], (count - lobby.startRowAt) * sizeof(e[0]));
		e[lobby.startRowAt] = lobby.startRow;
		count++;
	}
	// (the rows after the first placed again, one under the other, as the page has them at first)
	for (int32 i = 1; i < count; i++)
		e[i].m_X = e[i].m_Y = 0;
	CMenuManager &m = FrontEndMenuManager;
	if (m.m_nCurrScreen == MENUPAGE_START_MENU && m.m_nCurrOption >= count)
		m.m_nCurrOption = count - 1;
}

// ---------------------------------------------------------------- the frontend
void
Lobby_ScreenChanged(int32 screen)
{
	CMenuManager &m = FrontEndMenuManager;
	const int32 prev = m.m_nPrevScreen;
	if (lobby.editing && screen != MENUPAGE_MMO_NEW)
		StopEditing(true);
	// (the appearance picker left, whatever the way: its models freed, the menu's navigation its own again)
	if (screen != MENUPAGE_MMO_SKINS) {
		Skins_PickerClose();
		if (m.pEditString == &lobby.pickerDummy)
			m.pEditString = nil;
	}

	switch (screen) {
	case MENUPAGE_START_MENU:
		SyncStartRow();
		// (the lobby left: its link closed, unless a world is being entered)
		if (OurPage(prev) && lobby.entered && !lobby.joining && (Mode() == MODE_LOBBY || Mode() == MODE_OFF)) {
			lobby.entered = false;
			Disconnect();
			if (Mode() == MODE_LOBBY)
				SetMode(MODE_OFF);
		}
		break;

	case MENUPAGE_MMO_CHARACTERS:
		if (!OurPage(prev)) {
			// (entered from elsewhere: no request of ours pending any more; from the main menu, a fresh link, so a fresh
			// list — another page, e.g. the story's after a world failed to load, keeps its message)
			const LobbyData &l = Lobby();
			lobby.seenLobby = l.serial;
			lobby.seenResult = l.resultSerial;
			lobby.joining = lobby.creating = lobby.deleting = false;
			lobby.pendingOpenId = 0;
			lobby.signInAt = 0.0;
			if (prev == MENUPAGE_START_MENU)
				SetMessage(M_NONE, -1);
			if (Mode() == MODE_OFF)
				SetMode(MODE_LOBBY);
			if (NetAvailable()) {
				if (LinkState() == LINK_OPEN && prev == MENUPAGE_START_MENU)
					Disconnect();
				StartConnecting(false);
			}
			lobby.entered = true;
		}
		BuildList(ListStatus());
		if (prev == MENUPAGE_MMO_CHARACTER || prev == MENUPAGE_MMO_DELETE) {
			int32 index = -1;
			FindCharacter(lobby.selectedId, &index);
			SelectRow(screen, R_CHARACTER, index);
		} else if (prev == MENUPAGE_MMO_NEW)
			SelectRow(screen, R_NEW, -1);
		else
			SelectRow(screen, R_CHARACTER, 0);
		break;

	case MENUPAGE_MMO_CHARACTER:
		BuildCharacter();
		SelectRow(screen, prev == MENUPAGE_MMO_DELETE ? R_DELETE : prev == MENUPAGE_MMO_SKINS ? R_APPEARANCE : R_PLAY, -1);
		break;

	case MENUPAGE_MMO_NEW:
		BuildNew();
		if (prev == MENUPAGE_MMO_SKINS) {
			SelectRow(screen, R_APPEARANCE, -1);
			break;
		}
		SelectRow(screen, R_NAME, -1);
		// (an empty pseudo typed at once)
		if (lobby.newName[0] == '\0')
			StartEditing();
		break;

	case MENUPAGE_MMO_SKINS:
		BuildSkins();
		// (opened without a page of ours behind it, never by the menus: back to the list)
		if (!Skins_PickerOpenNow())
			lobby.pendingScreen = MENUPAGE_MMO_CHARACTERS;
		m.pEditString = &lobby.pickerDummy;
		break;

	case MENUPAGE_MMO_DELETE:
		BuildDelete();
		SelectRow(screen, R_NO, -1);
		break;

	case MENUPAGE_NEW_GAME:
		// (the world's load failed — the frontend shows the story's page then —: back to the characters; only the shared
		// world's load, a WELCOME's, leaves the session joining)
		if (prev == MENUPAGE_LOADING_IN_PROGRESS && Mode() == MODE_JOINING && m.m_bGameNotLoaded) {
			lobby.joining = false;
			SetMessage(M_LOAD_FAILED, MENUPAGE_MMO_CHARACTERS);
			lobby.pendingScreen = MENUPAGE_MMO_CHARACTERS;
		}
		break;
	}
}

void
Lobby_Changed(void)
{
	PollLobby();
	SettlePending();
}

void
Lobby_FrontendFrame(void)
{
	CMenuManager &m = FrontEndMenuManager;
	SettlePending();
	SyncStartRow();
	if (lobby.pendingScreen) {
		const int32 screen = lobby.pendingScreen;
		lobby.pendingScreen = 0;
		if (m.m_bMenuActive && m.m_nCurrScreen != screen) {
			m.SwitchToNewScreen(screen);
			return;
		}
	}
	if (!OurPage(m.m_nCurrScreen)) {
		if (lobby.editing)
			StopEditing(true);
		return;
	}
	PollLobby();
	const int32 page = m.m_nCurrScreen;

	// (the page left behind when the link's state no longer lets it be: the list tells why)
	if (page != MENUPAGE_MMO_CHARACTERS && ListStatus() != ST_LIST) {
		lobby.pendingScreen = MENUPAGE_MMO_CHARACTERS;
		return;
	}
	if (page == MENUPAGE_MMO_SKINS) {
		if (m.pEditString == nil)
			m.pEditString = &lobby.pickerDummy;
		Skins_PickerFrame();
	}
	if (page == MENUPAGE_MMO_CHARACTERS) {
		// (after a sign-in, the link tried again now and then until it is accepted)
		if (ListStatus() == ST_SIGN_IN && lobby.signInAt > 0.0 && Now() - lobby.signInAt < SIGN_IN_RETRY_FOR_MS &&
		    Now() - lobby.lastRetry > SIGN_IN_RETRY_MS && LinkState() == LINK_REFUSED)
			StartConnecting(true);
		RefreshList();
	}

	// (the text field: lost with the menu, or ended by a click elsewhere)
	if (lobby.editing) {
		if (m.pEditString != &lobby.editDummy || page != MENUPAGE_MMO_NEW)
			lobby.editing = false;
		else if (CPad::GetPad(0)->GetLeftMouseJustDown())
			StopEditing(true);
	}

	// (the cursor never left on a page's label — the frontend's going down wraps there —; the helper for left / right on
	// the appearance)
	const int32 p = page - PAGE_FIRST;
	if (m.m_nCurrOption >= 0 && m.m_nCurrOption < NUM_MENUROWS) {
		if (lobby.rows[p][m.m_nCurrOption].role == R_LABEL && lobby.numRows[p] > 1)
			m.m_nCurrOption = FirstOption(page);
		if (aScreens[page].layout)
			aScreens[page].layout->showLeftRightHelper = false;
	}
}

// (beyond the game's own rows, whose labels and right-hand texts say the rest: the appearance picker)
void
Lobby_DrawFrontend(int32 screen, bool active)
{
	if (screen == MENUPAGE_MMO_SKINS)
		Skins_PickerDraw(active);
}

// ---------------------------------------------------------------- the pages' texts
static const char *
MessageText(int32 message)
{
	switch (message) {
	case M_INVALID_NAME: return Tr("Invalid name: 2 to 16 letters, digits, spaces, _ . - (a letter or a digit first).",
	                               "Nom invalide : 2 à 16 lettres, chiffres, espaces, _ . - (une lettre ou un chiffre d'abord).");
	case M_NAME_TAKEN: return Tr("Name taken: choose another one.", "Nom déjà pris : choisissez-en un autre.");
	case M_TOO_MANY: return Tr("Too many characters: delete one first.", "Trop de personnages : supprimez-en un d'abord.");
	case M_NOT_FOUND: return Tr("This character no longer exists.", "Ce personnage n'existe plus.");
	case M_WORLD_FULL: return Tr("Server full: try again later.", "Serveur complet : réessayez plus tard.");
	case M_ERROR: return Tr("Server error: try again later.", "Erreur du serveur : réessayez plus tard.");
	case M_INVALID_SKIN: return Tr("This appearance is not allowed.", "Cette apparence n'est pas autorisée.");
	case M_SKIN_SAVED: return Tr("Appearance saved.", "Apparence enregistrée.");
	case M_DELETED: return Tr("Character deleted.", "Personnage supprimé.");
	case M_LOAD_FAILED: return Tr("The shared world could not be loaded.", "Le monde partagé n'a pas pu être chargé.");
	}
	return "";
}

static const char *
StatusText(int32 status)
{
	switch (status) {
	case ST_CONNECTING: return Tr("Connecting to the shared world...", "Connexion au monde partagé...");
	case ST_NO_SERVER: return Tr("The shared world's server does not answer; Orthros keeps trying.",
	                             "Le serveur du monde partagé ne répond pas ; Orthros réessaie.");
	case ST_SIGN_IN: return Tr("Sign in to your Orthros account to play online.", "Connectez-vous à votre compte Orthros pour jouer en ligne.");
	case ST_UNAVAILABLE: return Tr("Multiplayer unavailable.", "Multijoueur indisponible.");
	case ST_OUTDATED: return Tr("Multiplayer unavailable: reload the page to update the game.",
	                            "Multijoueur indisponible : rechargez la page pour mettre le jeu à jour.");
	case ST_FULL: return Tr("Server full: try again later.", "Serveur complet : réessayez plus tard.");
	case ST_ELSEWHERE: return Tr("Your character entered the shared world from another window.",
	                             "Votre personnage est entré dans le monde partagé depuis une autre fenêtre.");
	case ST_ERROR: return Tr("Server error: try again later.", "Erreur du serveur : réessayez plus tard.");
	}
	return "";
}

// (the label of the page shown — or of the one fading out, the frontend drawing it as the current one meanwhile)
static void
LabelText(char *out, int32 size)
{
	const int32 page = FrontEndMenuManager.m_nCurrScreen;
	const LobbyData &l = Lobby();
	out[0] = '\0';
	switch (page) {
	case MENUPAGE_MMO_CHARACTERS: {
		const int32 status = ListStatus();
		if (status != ST_LIST)
			snprintf(out, size, "%s", StatusText(status));
		else if (lobby.deleting)
			snprintf(out, size, "%s", Tr("Deleting the character...", "Suppression du personnage..."));
		else if (MessageOn(page))
			snprintf(out, size, "%s", MessageText(MessageOn(page)));
		else
			snprintf(out, size, l.count > 1 ? Tr("Account %s: %d of %d characters%s.", "Compte %s : %d personnages sur %d%s.")
			                                : Tr("Account %s: %d of %d character%s.", "Compte %s : %d personnage sur %d%s."), l.username,
			         l.count, l.maxCharacters, l.admin ? Tr(" (administrator)", " (administrateur)") : "");
		break;
	}
	case MENUPAGE_MMO_CHARACTER: {
		const LobbyCharacter *c = FindCharacter(lobby.selectedId);
		if (lobby.joining)
			snprintf(out, size, "%s", Tr("Entering the shared world...", "Entrée dans le monde partagé..."));
		else if (MessageOn(page))
			snprintf(out, size, "%s", MessageText(MessageOn(page)));
		else if (c)
			snprintf(out, size, Tr("Money: $%d%s", "Argent : $%d%s"), c->money,
			         c->online ? Tr("  -  in the shared world now", "  -  dans le monde partagé en ce moment") : "");
		break;
	}
	case MENUPAGE_MMO_NEW:
		if (lobby.editing)
			snprintf(out, size, "%s", Tr("Type the pseudo, then Enter (Esc: cancel). 2 to 16 letters, digits, spaces, _ . -",
			                             "Tapez le pseudo, puis Entrée (Echap : annuler). 2 à 16 lettres, chiffres, espaces, _ . -"));
		else if (lobby.creating)
			snprintf(out, size, "%s", Tr("Creating the character...", "Création du personnage..."));
		else if (MessageOn(page))
			snprintf(out, size, "%s", MessageText(MessageOn(page)));
		else if (lobby.newName[0] == '\0')
			snprintf(out, size, "%s", Tr("Choose a pseudo and an appearance.", "Choisissez un pseudo et une apparence."));
		else if (!ValidName(lobby.newName))
			snprintf(out, size, "%s", MessageText(M_INVALID_NAME));
		else
			snprintf(out, size, "%s", Tr("A new character starts with $5000 and a pistol, at the Ocean View hotel.",
			                             "Un nouveau personnage commence avec 5000 $ et un pistolet, à l'hôtel Ocean View."));
		break;
	}
}

wchar *
Lobby_Text(const char *key)
{
	static const struct { const char *key, *en, *fr; } texts[] = {
		{ "ORT_MP", "Multiplayer", "Multijoueur" },
		{ "ORT_NCH", "New character", "Nouveau personnage" },
		{ "ORT_SIN", "Sign in", "Se connecter" },
		{ "ORT_RTY", "Retry", "Réessayer" },
		{ "ORT_PLY", "Play", "Jouer" },
		{ "ORT_APP", "Appearance", "Apparence" },
		{ "ORT_APT", "Appearance", "Apparence" },
		{ "ORT_DEL", "Delete", "Supprimer" },
		{ "ORT_NAM", "Pseudo", "Pseudo" },
		{ "ORT_CRE", "Create", "Créer" },
	};
	static wchar fixed[ARRAY_SIZE(texts)][2][40];
	static wchar names[MAX_LIST_CHARS][24], title[24], label[256], question[160];
	if (strncmp(key, "ORT_", 4) != 0)
		return nil;

	for (int32 i = 0; i < (int32)ARRAY_SIZE(texts); i++)
		if (strcmp(key, texts[i].key) == 0) {
			const int32 lang = LangFrench() ? 1 : 0;
			if (fixed[i][lang][0] == 0)
				Game(lang ? texts[i].fr : texts[i].en, fixed[i][lang], ARRAY_SIZE(fixed[i][lang]));
			return fixed[i][lang];
		}

	// (a character of the list: its name)
	if (key[4] == 'C' && key[5] >= '1' && key[5] <= '9') {
		const int32 i = atoi(key + 5) - 1;
		if (i < 0 || i >= MAX_LIST_CHARS)
			return nil;
		const LobbyData &l = Lobby();
		return Game(i < l.count ? l.chars[i].name : "", names[i], ARRAY_SIZE(names[i]));
	}
	if (strcmp(key, "ORT_CHN") == 0) {
		const LobbyCharacter *c = FindCharacter(lobby.selectedId);
		return Game(c ? c->name : "", title, ARRAY_SIZE(title));
	}
	if (strcmp(key, "ORT_STA") == 0) {
		char utf8[256];
		LabelText(utf8, sizeof(utf8));
		return Game(utf8, label, ARRAY_SIZE(label));
	}
	if (strcmp(key, "ORT_DLQ") == 0) {
		const LobbyCharacter *c = FindCharacter(lobby.selectedId);
		char utf8[160];
		snprintf(utf8, sizeof(utf8), Tr("Delete %s? Its money, weapons and position will be lost for good.",
		                                 "Supprimer %s ? Son argent, ses armes et sa position seront perdus pour de bon."), c ? c->name : "");
		return Game(utf8, question, ARRAY_SIZE(question));
	}
	return nil;
}

// ---------------------------------------------------------------- tests
static const char *
PageName(int32 screen)
{
	switch (screen) {
	case MENUPAGE_MMO_CHARACTERS: return "characters";
	case MENUPAGE_MMO_CHARACTER: return "character";
	case MENUPAGE_MMO_NEW: return "new";
	case MENUPAGE_MMO_DELETE: return "delete";
	case MENUPAGE_MMO_SKINS: return "skins";
	case MENUPAGE_START_MENU: return "start";
	}
	return "other";
}

// (a JSON string's characters: names and texts here are ASCII, quotes and backslashes escaped)
static void
JsonText(const char *s, char *out, int32 size)
{
	int32 n = 0;
	for (; *s && n < size - 3; s++) {
		const uint8 c = *s;
		if (c == '"' || c == '\\')
			out[n++] = '\\';
		out[n++] = c >= 0x20 && c < 0x7F ? c : '?';
	}
	out[n] = '\0';
}

void
Lobby_Debug(void)
{
	const CMenuManager &m = FrontEndMenuManager;
	const int32 page = m.m_nCurrScreen;
	char label[256], json[300], rows[NUM_MENUROWS * 12 + 4], name[40];
	LabelText(label, sizeof(label));
	JsonText(label, json, sizeof(json));
	JsonText(lobby.newName, name, sizeof(name));
	int32 n = 0;
	rows[n++] = '[';
	if (OurPage(page) || page == MENUPAGE_START_MENU)
		for (int32 i = 0; i < NUM_MENUROWS && aScreens[page].m_aEntries[i].m_Action != MENUACTION_NOTHING; i++)
			n += snprintf(rows + n, sizeof(rows) - n, "%s\"%.7s\"", i ? "," : "", aScreens[page].m_aEntries[i].m_EntryName);
	snprintf(rows + n, sizeof(rows) - n, "]");
	const LobbyCharacter *c = FindCharacter(lobby.selectedId);
	DebugAppend(",\"lobbyMenu\":{\"page\":\"%s\",\"status\":\"%s\",\"label\":\"%s\",\"rows\":%s,\"cursor\":%d,\"selected\":\"%s\","
	            "\"skin\":\"%s\",\"newName\":\"%s\",\"newSkin\":\"%s\",\"editing\":%s,\"creating\":%s,\"deleting\":%s,\"joining\":%s,"
	            "\"message\":\"%s\"}",
	            PageName(page), statusNames[ListStatus()], json, rows, m.m_nCurrOption, c ? c->name : "",
	            lobby.skinChoice, name, lobby.newSkin[0] ? lobby.newSkin : "player",
	            lobby.editing ? "true" : "false", lobby.creating ? "true" : "false", lobby.deleting ? "true" : "false",
	            lobby.joining ? "true" : "false", messageNames[lobby.message]);
	Skins_Debug();
}

// (the word after a command, and the rest after it)
static const char *
Arg(const char *cmd, char *word, int32 size)
{
	while (*cmd == ' ')
		cmd++;
	int32 n = 0;
	for (; *cmd && *cmd != ' '; cmd++)
		if (n < size - 1)
			word[n++] = *cmd;
	word[n] = '\0';
	while (*cmd == ' ')
		cmd++;
	return cmd;
}

// "lobby": the lobby's state printed. "mpmenu": the multiplayer page opened from the main menu. "mpcreate <name>
// [skin]", "mpplay <name>", "mpskin <name> <skin>", "mpdelete <name>": the pages' own requests, by name. The pages
// driven as a player does: "mpopen <name>" (a character's page), "mpnew" (the new character page), "mpname <pseudo>"
// (typed there), "mpskins" (the appearance picker from either page; its own commands: mmo_skins.cpp).
bool
Lobby_TestCommand(const char *cmd)
{
	char word[24], name[24], skin[24];
	const char *rest = Arg(cmd, word, sizeof(word));
	const CMenuManager &m = FrontEndMenuManager;
	if (strcmp(word, "lobby") == 0) {
		const LobbyData &l = Lobby();
		debug("lobby: page %s, status %s, link %d, refusal %d, mode %d, account '%s'%s, %d/%d characters\n", PageName(m.m_nCurrScreen),
		      statusNames[ListStatus()], LinkState(), Refusal(), Mode(), l.username, l.admin ? " (admin)" : "", l.count, l.maxCharacters);
		for (int32 i = 0; i < l.count && i < MAX_LIST_CHARS; i++)
			debug("lobby:   %u '%s' %s $%d%s\n", l.chars[i].id, l.chars[i].name, l.chars[i].skin, l.chars[i].money, l.chars[i].online ? " online" : "");
		char label[256];
		LabelText(label, sizeof(label));
		debug("lobby: label '%s', message %s, editing %d, creating %d, deleting %d, joining %d\n", label, messageNames[lobby.message],
		      lobby.editing, lobby.creating, lobby.deleting, lobby.joining);
		return true;
	}
	if (strcmp(word, "mpmenu") == 0) {
		if (m.m_bMenuActive && m.m_bGameNotLoaded)
			lobby.pendingScreen = MENUPAGE_MMO_CHARACTERS;
		else
			debug("mpmenu: only from the main menu\n");
		return true;
	}
	if (strcmp(word, "mpcreate") == 0) {
		rest = Arg(rest, name, sizeof(name));
		Arg(rest, skin, sizeof(skin));
		const char *model = skin[0] ? Lobby_SkinModel(skin) : "player";
		if (model == nil || !LobbyCreate(name, model))
			debug("mpcreate: refused (%s)\n", model == nil ? "unknown skin" : messageNames[lobby.message]);
		return true;
	}
	if (strcmp(word, "mpskins") == 0) {
		// (the appearance picker, as the Appearance row opens it, from the character page or the new character page)
		const LobbyCharacter *c = FindCharacter(lobby.selectedId);
		if (!m.m_bMenuActive || (m.m_nCurrScreen != MENUPAGE_MMO_CHARACTER && m.m_nCurrScreen != MENUPAGE_MMO_NEW))
			debug("mpskins: only from the character or new character page\n");
		else if (m.m_nCurrScreen == MENUPAGE_MMO_NEW) {
			StopEditing(true);
			OpenPicker(MENUPAGE_MMO_NEW, lobby.newSkin[0] ? lobby.newSkin : "player");
		} else
			OpenPicker(MENUPAGE_MMO_CHARACTER, lobby.skinChoice[0] ? lobby.skinChoice : c ? c->skin : nil);
		return true;
	}
	if (strcmp(word, "mpnew") == 0) {
		// (the new character page, from the characters' list)
		if (m.m_bMenuActive && m.m_nCurrScreen == MENUPAGE_MMO_CHARACTERS && LinkStatus() == ST_LIST)
			lobby.pendingScreen = MENUPAGE_MMO_NEW;
		else
			debug("mpnew: only from the characters' list\n");
		return true;
	}
	if (strcmp(word, "mpname") == 0) {
		// (the new character's pseudo, as typed)
		if (m.m_nCurrScreen != MENUPAGE_MMO_NEW)
			debug("mpname: only on the new character page\n");
		else {
			StopEditing(true);
			snprintf(lobby.newName, sizeof(lobby.newName), "%s", rest);
		}
		return true;
	}
	if (strcmp(word, "mpopen") == 0) {
		// (a character's page, as its row opens it)
		Arg(rest, name, sizeof(name));
		const LobbyCharacter *c = FindCharacterNamed(name);
		if (c == nil || !m.m_bMenuActive || !OurPage(m.m_nCurrScreen))
			debug("mpopen: no character '%s' here\n", name);
		else {
			lobby.selectedId = c->id;
			SetSkin(lobby.skinChoice, c->skin);
			SetMessage(M_NONE, -1);
			lobby.pendingScreen = MENUPAGE_MMO_CHARACTER;
		}
		return true;
	}
	if (Skins_TestCommand(word, rest))
		return true;
	if (strcmp(word, "mpplay") == 0 || strcmp(word, "mpskin") == 0 || strcmp(word, "mpdelete") == 0) {
		rest = Arg(rest, name, sizeof(name));
		Arg(rest, skin, sizeof(skin));
		const LobbyCharacter *c = FindCharacterNamed(name);
		if (c == nil) {
			debug("%s: no character '%s'\n", word, name);
			return true;
		}
		lobby.selectedId = c->id;
		if (strcmp(word, "mpplay") == 0) {
			SetSkin(lobby.skinChoice, c->skin);
			LobbyPlay(c->id);
		} else if (strcmp(word, "mpskin") == 0) {
			if (Lobby_SkinModel(skin) == nil)
				debug("mpskin: unknown skin '%s'\n", skin);
			else {
				SetSkin(lobby.skinChoice, skin);
				ApplySkinChoice();
			}
		} else
			LobbyDelete(c->id);
		return true;
	}
	return false;
}
}
#endif
