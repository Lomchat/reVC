// The shared world's skins (docs/mmo.md, section 7 "skins"): what a character may wear and the lobby's appearance
// picker.
//   - The list: Tommy's outfits, the story's characters and the game's other special characters (DFFs of
//     models/gta3.img without an IDE entry, with the in-game skeleton: 24 bones; the cutscenes' versions mostly have
//     26 to 53 and are left out; each looked at: duplicates of another skin and models holding a weapon left out),
//     every pedestrian of default.ide (police and services, gangs, the city's people). Each with its
//     category, its gender and its English and French names; a skin is its model's name (the server's rule:
//     [A-Za-z0-9_]{1,20}), worn by the session as a special model (by name, in a special slot) or as an IDE model (by
//     index), as before.
//   - The picker: the page MENUPAGE_MMO_SKINS, opened from the Appearance row of the character page and of the new
//     character page. A filter (Men / Women / All), the list sorted by category then name (Tommy's outfits in the game's
//     order) and scrolled with the keys, the wheel and the mouse, the selected skin in the menus' green, the
//     character's own marked; a 3D preview of the selected skin, turning slowly in its idle stance, its name under it.
//     Enter (or a click on the selected row, or Choose) chooses it, Esc (or Back) goes back.
//   - The preview runs in the main menu, before any game is loaded: no streaming, no model infos, no animations then. It
//     reads the game's files itself and never blocks a frame: each read is asked of the page first (OrthrosFileReady:
//     fetched in the background) and done once its bytes are here: models/gta3.dir (the image's directory), the
//     skin's DFF and TXD in models/gta3.img, anim/ped.ifp (the idle stances, men's and women's); a placeholder
//     meanwhile. A few models are kept while the page is open (going back over the list is instant), the neighbours'
//     bytes asked ahead; everything is freed when the page closes.
// Test commands (lobby): "mpskins" (the picker opened from the character or new character page shown), "mpfilter
// male|female|all", "mppick <model>", "mpconfirm", "mpcancel", "mpturn <degrees>|off" (the preview's turn held: 0 faces
// the viewer), "mpskinlist" (the list printed). Debug state: "skins":
// { open, filter, rows, count, selected, current, first, preview: none|loading|ready|failed, cached, anim }.
#include "common.h"

#ifdef ORTHROS_EXTRAS
#include <fcntl.h>
#include <unistd.h>
#include "mmo_internal.h"
#include "crossplatform.h"
#include "main.h"
#include "Frontend.h"
#include "Text.h"
#include "Pad.h"
#include "Font.h"
#include "Sprite2d.h"
#include "Timer.h"
#include "Lights.h"
#include "Quaternion.h"
#include "MemoryMgr.h"
#include "RwHelper.h"
#include "rwcore.h"
#include "rphanim.h"
#include "rpskin.h"
#include "skeleton.h"
#ifdef __EMSCRIPTEN__
#include "orthros_web.h"
#endif

namespace OrthrosMmo
{
// ---------------------------------------------------------------- the list
enum { CAT_TOMMY, CAT_STORY, CAT_LAW, CAT_GANGS, CAT_ROLES, CAT_PEOPLE, NUM_CATS };
enum { G_MALE, G_FEMALE };
struct SkinDef { const char *model; int8 cat, gender, outfit; const char *en, *fr; };

// Tommy's outfits are named by the game's own texts (OUTFT1..12: its clothes pickups' numbers, the wardrobe's models in
// main.scm); OUTFT13 (MC Tommy) has no model.
static const SkinDef skins[] = {
	{ "player", CAT_TOMMY, G_MALE, 1, nil, nil },
	{ "player2", CAT_TOMMY, G_MALE, 2, nil, nil },
	{ "player3", CAT_TOMMY, G_MALE, 3, nil, nil },
	{ "player4", CAT_TOMMY, G_MALE, 4, nil, nil },
	{ "player5", CAT_TOMMY, G_MALE, 5, nil, nil },
	{ "player6", CAT_TOMMY, G_MALE, 6, nil, nil },
	{ "player7", CAT_TOMMY, G_MALE, 7, nil, nil },
	{ "player8", CAT_TOMMY, G_MALE, 8, nil, nil },
	{ "player9", CAT_TOMMY, G_MALE, 9, nil, nil },
	{ "play10", CAT_TOMMY, G_MALE, 10, nil, nil },
	{ "play11", CAT_TOMMY, G_MALE, 11, nil, nil },
	{ "play12", CAT_TOMMY, G_MALE, 12, nil, nil },

	// the story's characters (special models)
	{ "IGbuddy", CAT_STORY, G_MALE, 0, "Lance Vance", "Lance Vance" },
	{ "IGBudy2", CAT_STORY, G_MALE, 0, "Lance Vance (2)", "Lance Vance (2)" },
	{ "IGBudy3", CAT_STORY, G_MALE, 0, "Lance Vance (3)", "Lance Vance (3)" },
	{ "IGKen", CAT_STORY, G_MALE, 0, "Ken Rosenberg", "Ken Rosenberg" },
	{ "IGDiaz", CAT_STORY, G_MALE, 0, "Ricardo Diaz", "Ricardo Diaz" },
	{ "IGSonny", CAT_STORY, G_MALE, 0, "Sonny Forelli", "Sonny Forelli" },
	{ "IGmike", CAT_STORY, G_MALE, 0, "Mike Forelli", "Mike Forelli" },
	{ "IGMike2", CAT_STORY, G_MALE, 0, "Mike Forelli (2)", "Mike Forelli (2)" },
	{ "IGColon", CAT_STORY, G_MALE, 0, "Colonel Cortez", "Colonel Cortez" },
	{ "IGMerc", CAT_STORY, G_FEMALE, 0, "Mercedes Cortez", "Mercedes Cortez" },
	{ "IGMerc2", CAT_STORY, G_FEMALE, 0, "Mercedes Cortez (2)", "Mercedes Cortez (2)" },
	{ "IGCandy", CAT_STORY, G_FEMALE, 0, "Candy Suxxx", "Candy Suxxx" },
	{ "IGHlary", CAT_STORY, G_MALE, 0, "Hilary King", "Hilary King" },
	{ "IGHlry2", CAT_STORY, G_MALE, 0, "Hilary King (2)", "Hilary King (2)" },
	{ "IGPhil", CAT_STORY, G_MALE, 0, "Phil Cassidy", "Phil Cassidy" },
	{ "IGphil2", CAT_STORY, G_MALE, 0, "Phil Cassidy (2)", "Phil Cassidy (2)" },
	{ "IGPhil3", CAT_STORY, G_MALE, 0, "Phil Cassidy (3)", "Phil Cassidy (3)" },
	{ "IGJezz", CAT_STORY, G_MALE, 0, "Jezz Torrent", "Jezz Torrent" },
	{ "IGDick", CAT_STORY, G_MALE, 0, "Dick (Love Fist)", "Dick (Love Fist)" },
	{ "IGPercy", CAT_STORY, G_MALE, 0, "Percy (Love Fist)", "Percy (Love Fist)" },
	{ "IGGonz", CAT_STORY, G_MALE, 0, "Gonzalez", "Gonzalez" },
	{ "IGalscb", CAT_STORY, G_MALE, 0, "Alex Shrub", "Alex Shrub" },
	{ "kentpaul", CAT_STORY, G_MALE, 0, "Kent Paul", "Kent Paul" },
	{ "lawyer", CAT_STORY, G_MALE, 0, "Ken Rosenberg (2)", "Ken Rosenberg (2)" },
	{ "colonel", CAT_STORY, G_MALE, 0, "Colonel Cortez (2)", "Colonel Cortez (2)" },
	{ "buddy", CAT_STORY, G_MALE, 0, "Lance Vance (4)", "Lance Vance (4)" },

	// the missions' other characters (special models). Left out: the ones that look exactly like a pedestrian below or
	// like another one here (bounca, C_worker, cdrivra, cdrivrb, cmraman, crewa, crewb, cservrb, dgoonc, floozya,
	// floozyb, jzgirlb, KgoonA, Mba, Mbb, mporna, mserver, printrc, shootra, shootrb, and the cutscenes' CSgona,
	// CScgona, CSgangc, CSdisp, CSpablo, CSFiller, CSPepe, CSJuggz), and those holding a weapon in their own mesh
	// (CSassA, CSassB, CSassC)
	{ "bga", CAT_ROLES, G_MALE, 0, "Bodyguard", "Garde du corps" },
	{ "bgb", CAT_ROLES, G_MALE, 0, "Bodyguard (2)", "Garde du corps (2)" },
	{ "burger", CAT_ROLES, G_MALE, 0, "Fast food cook", "Cuisinier de fast-food" },
	{ "cgona", CAT_ROLES, G_MALE, 0, "Sailor", "Marin" },
	{ "cgonb", CAT_ROLES, G_MALE, 0, "Sailor (2)", "Marin (2)" },
	{ "cgonc", CAT_ROLES, G_MALE, 0, "Sailor (3)", "Marin (3)" },
	{ "chef", CAT_ROLES, G_MALE, 0, "Chef", "Chef cuisinier" },
	{ "courier", CAT_ROLES, G_MALE, 0, "Courier", "Coursier" },
	{ "cservra", CAT_ROLES, G_FEMALE, 0, "Waitress", "Serveuse" },
	{ "floozyc", CAT_ROLES, G_FEMALE, 0, "Partygoer", "Invitée de soirée" },
	{ "jzgirla", CAT_ROLES, G_FEMALE, 0, "Girl in a sundress", "Fille en robe d'été" },
	{ "printra", CAT_ROLES, G_MALE, 0, "Print works worker", "Ouvrier d'imprimerie" },
	{ "printrb", CAT_ROLES, G_MALE, 0, "Print works worker (2)", "Ouvrier d'imprimerie (2)" },
	{ "psycho", CAT_ROLES, G_FEMALE, 0, "Groupie", "Groupie" },
	{ "S_keep", CAT_ROLES, G_MALE, 0, "Shopkeeper", "Commerçant" },
	{ "sam", CAT_ROLES, G_MALE, 0, "Student", "Étudiant" },
	{ "spandxa", CAT_ROLES, G_MALE, 0, "Workman", "Ouvrier" },
	{ "spandxb", CAT_ROLES, G_MALE, 0, "Workman (2)", "Ouvrier (2)" },
	{ "Stripa", CAT_ROLES, G_FEMALE, 0, "Stripper", "Strip-teaseuse" },
	{ "stripb", CAT_ROLES, G_FEMALE, 0, "Stripper (2)", "Strip-teaseuse (2)" },
	{ "stripc", CAT_ROLES, G_FEMALE, 0, "Stripper (3)", "Strip-teaseuse (3)" },
	{ "taxi_d", CAT_ROLES, G_MALE, 0, "Old cabbie", "Vieux taxi" },
	{ "wfycst", CAT_ROLES, G_FEMALE, 0, "Lady in black", "Dame en noir" },
	{ "cgoona", CAT_GANGS, G_MALE, 0, "Gangster", "Gangster" },
	{ "dgoona", CAT_GANGS, G_MALE, 0, "Diaz's gang (3)", "Gang de Diaz (3)" },
	{ "dgoonb", CAT_GANGS, G_MALE, 0, "Diaz's gang (4)", "Gang de Diaz (4)" },
	{ "mgoona", CAT_GANGS, G_MALE, 0, "Biker (3)", "Motard (3)" },
	{ "sgc", CAT_GANGS, G_MALE, 0, "Streetwannabe (3)", "Streetwannabe (3)" },
	{ "sgoona", CAT_GANGS, G_MALE, 0, "Streetwannabe (4)", "Streetwannabe (4)" },
	{ "sgoonb", CAT_GANGS, G_MALE, 0, "Streetwannabe (5)", "Streetwannabe (5)" },
	{ "FSfa", CAT_LAW, G_MALE, 0, "Special forces", "Forces spéciales" },

	// police and services (default.ide)
	{ "cop", CAT_LAW, G_MALE, 0, "Police officer", "Policier" },
	{ "swat", CAT_LAW, G_MALE, 0, "SWAT officer", "Agent du SWAT" },
	{ "fbi", CAT_LAW, G_MALE, 0, "FBI agent", "Agent du FBI" },
	{ "army", CAT_LAW, G_MALE, 0, "Soldier", "Soldat" },
	{ "medic", CAT_LAW, G_MALE, 0, "Paramedic", "Ambulancier" },
	{ "fireman", CAT_LAW, G_MALE, 0, "Firefighter", "Pompier" },
	// (the vice squad's plain clothes: vice7 and vice8 look exactly as vice1 and vice2; GDb as GDa)
	{ "vice1", CAT_LAW, G_MALE, 0, "Undercover cop", "Flic en civil" },
	{ "vice2", CAT_LAW, G_MALE, 0, "Undercover cop (2)", "Flic en civil (2)" },
	{ "vice3", CAT_LAW, G_MALE, 0, "Undercover cop (3)", "Flic en civil (3)" },
	{ "vice4", CAT_LAW, G_MALE, 0, "Undercover cop (4)", "Flic en civil (4)" },
	{ "vice5", CAT_LAW, G_MALE, 0, "Undercover cop (5)", "Flic en civil (5)" },
	{ "vice6", CAT_LAW, G_MALE, 0, "Undercover cop (6)", "Flic en civil (6)" },
	{ "GDa", CAT_LAW, G_MALE, 0, "Security guard", "Vigile" },

	// gangs (default.ide)
	{ "CBa", CAT_GANGS, G_MALE, 0, "Cuban gang", "Gang cubain" },
	{ "CBb", CAT_GANGS, G_MALE, 0, "Cuban gang (2)", "Gang cubain (2)" },
	{ "HNa", CAT_GANGS, G_MALE, 0, "Haitian gang", "Gang haïtien" },
	{ "HNb", CAT_GANGS, G_MALE, 0, "Haitian gang (2)", "Gang haïtien (2)" },
	{ "SGa", CAT_GANGS, G_MALE, 0, "Streetwannabe", "Streetwannabe" },
	{ "SGb", CAT_GANGS, G_MALE, 0, "Streetwannabe (2)", "Streetwannabe (2)" },
	{ "CLa", CAT_GANGS, G_MALE, 0, "Diaz's gang", "Gang de Diaz" },
	{ "CLb", CAT_GANGS, G_MALE, 0, "Diaz's gang (2)", "Gang de Diaz (2)" },
	{ "BKa", CAT_GANGS, G_MALE, 0, "Biker", "Motard" },
	{ "BKb", CAT_GANGS, G_MALE, 0, "Biker (2)", "Motard (2)" },
	{ "PGa", CAT_GANGS, G_MALE, 0, "Vercetti gang", "Gang Vercetti" },
	{ "PGb", CAT_GANGS, G_MALE, 0, "Vercetti gang (2)", "Gang Vercetti (2)" },

	// the city's people (default.ide; the gender is the IDE's: CIVFEMALE and PROSTITUTE are women). Left out: HFOTR
	// (it looks exactly as HFOST) and the shoppers WFYSH, WFOSH (their bags are part of their mesh).
	{ "male01", CAT_PEOPLE, G_MALE, 0, "Average Joe", "Monsieur Tout-le-monde" },
	{ "HMYST", CAT_PEOPLE, G_MALE, 0, "Street guy", "Gars du quartier" },
	{ "BMYST", CAT_PEOPLE, G_MALE, 0, "Street guy (2)", "Gars du quartier (2)" },
	{ "WMYST", CAT_PEOPLE, G_MALE, 0, "Street guy (3)", "Gars du quartier (3)" },
	// (no u-circumflex in these names: the menus' fonts have none)
	{ "HMOST", CAT_PEOPLE, G_MALE, 0, "Older man", "Homme d'un certain âge" },
	{ "BMOST", CAT_PEOPLE, G_MALE, 0, "Older man (2)", "Homme d'un certain âge (2)" },
	{ "WMOST", CAT_PEOPLE, G_MALE, 0, "Older man (3)", "Homme d'un certain âge (3)" },
	{ "HMYRI", CAT_PEOPLE, G_MALE, 0, "Rich guy", "Jeune homme chic" },
	{ "BMYRI", CAT_PEOPLE, G_MALE, 0, "Rich guy (2)", "Jeune homme chic (2)" },
	{ "WMYRI", CAT_PEOPLE, G_MALE, 0, "Rich guy (3)", "Jeune homme chic (3)" },
	{ "HMORI", CAT_PEOPLE, G_MALE, 0, "Rich gentleman", "Monsieur chic" },
	{ "WMORI", CAT_PEOPLE, G_MALE, 0, "Rich gentleman (2)", "Monsieur chic (2)" },
	{ "HMYBE", CAT_PEOPLE, G_MALE, 0, "Beach guy", "Baigneur" },
	{ "BMYBE", CAT_PEOPLE, G_MALE, 0, "Beach guy (2)", "Baigneur (2)" },
	{ "WMYBE", CAT_PEOPLE, G_MALE, 0, "Beach guy (3)", "Baigneur (3)" },
	{ "HMOBE", CAT_PEOPLE, G_MALE, 0, "Older beachgoer", "Baigneur âgé" },
	{ "BMOBE", CAT_PEOPLE, G_MALE, 0, "Older beachgoer (2)", "Baigneur âgé (2)" },
	{ "WMOBE", CAT_PEOPLE, G_MALE, 0, "Older beachgoer (3)", "Baigneur âgé (3)" },
	{ "BMYBU", CAT_PEOPLE, G_MALE, 0, "Businessman", "Homme d'affaires" },
	{ "WMYBU", CAT_PEOPLE, G_MALE, 0, "Businessman (2)", "Homme d'affaires (2)" },
	{ "WMOBU", CAT_PEOPLE, G_MALE, 0, "Businessman (3)", "Homme d'affaires (3)" },
	{ "HMOTR", CAT_PEOPLE, G_MALE, 0, "Tramp", "Clochard" },
	{ "BMOTR", CAT_PEOPLE, G_MALE, 0, "Tramp (2)", "Clochard (2)" },
	{ "WMOTR", CAT_PEOPLE, G_MALE, 0, "Tramp (3)", "Clochard (3)" },
	{ "HMYAP", CAT_PEOPLE, G_MALE, 0, "Mechanic", "Mécanicien" },
	{ "HMOCA", CAT_PEOPLE, G_MALE, 0, "Taxi driver", "Chauffeur de taxi" },
	{ "WMOCA", CAT_PEOPLE, G_MALE, 0, "Taxi driver (2)", "Chauffeur de taxi (2)" },
	{ "BMODK", CAT_PEOPLE, G_MALE, 0, "Docker", "Docker" },
	{ "BMYCR", CAT_PEOPLE, G_MALE, 0, "Thug", "Voyou" },
	{ "WMYCR", CAT_PEOPLE, G_MALE, 0, "Thug (2)", "Voyou (2)" },
	{ "BMYPI", CAT_PEOPLE, G_MALE, 0, "Pimp", "Proxénète" },
	{ "WMYPI", CAT_PEOPLE, G_MALE, 0, "Pimp (2)", "Proxénète (2)" },
	{ "BMYBB", CAT_PEOPLE, G_MALE, 0, "Street musician", "Musicien de rue" },
	{ "WMYCW", CAT_PEOPLE, G_MALE, 0, "Construction worker", "Ouvrier du bâtiment" },
	{ "WMYGO", CAT_PEOPLE, G_MALE, 0, "Golfer", "Golfeur" },
	{ "WMOGO", CAT_PEOPLE, G_MALE, 0, "Golfer (2)", "Golfeur (2)" },
	{ "WMYLG", CAT_PEOPLE, G_MALE, 0, "Lifeguard", "Maître-nageur" },
	{ "WMYJG", CAT_PEOPLE, G_MALE, 0, "Jogger", "Joggeur" },
	{ "WMYSK", CAT_PEOPLE, G_MALE, 0, "Roller skater", "Patineur" },
	{ "JMOTO", CAT_PEOPLE, G_MALE, 0, "Tourist", "Touriste" },
	{ "HFYST", CAT_PEOPLE, G_FEMALE, 0, "Street girl", "Passante" },
	{ "BFYST", CAT_PEOPLE, G_FEMALE, 0, "Street girl (2)", "Passante (2)" },
	{ "WFYST", CAT_PEOPLE, G_FEMALE, 0, "Street girl (3)", "Passante (3)" },
	{ "HFYCG", CAT_PEOPLE, G_FEMALE, 0, "Street girl (4)", "Passante (4)" },
	{ "HFOST", CAT_PEOPLE, G_FEMALE, 0, "Older woman", "Femme d'un certain âge" },
	{ "BFOST", CAT_PEOPLE, G_FEMALE, 0, "Older woman (2)", "Femme d'un certain âge (2)" },
	{ "WFOST", CAT_PEOPLE, G_FEMALE, 0, "Older woman (3)", "Femme d'un certain âge (3)" },
	{ "HFYRI", CAT_PEOPLE, G_FEMALE, 0, "Rich girl", "Jeune femme chic" },
	{ "BFYRI", CAT_PEOPLE, G_FEMALE, 0, "Rich girl (2)", "Jeune femme chic (2)" },
	{ "WFYRI", CAT_PEOPLE, G_FEMALE, 0, "Rich girl (3)", "Jeune femme chic (3)" },
	{ "HFORI", CAT_PEOPLE, G_FEMALE, 0, "Rich lady", "Dame chic" },
	{ "BFORI", CAT_PEOPLE, G_FEMALE, 0, "Rich lady (2)", "Dame chic (2)" },
	{ "WFORI", CAT_PEOPLE, G_FEMALE, 0, "Rich lady (3)", "Dame chic (3)" },
	{ "HFYBE", CAT_PEOPLE, G_FEMALE, 0, "Beach girl", "Baigneuse" },
	{ "BFYBE", CAT_PEOPLE, G_FEMALE, 0, "Beach girl (2)", "Baigneuse (2)" },
	{ "WFYBE", CAT_PEOPLE, G_FEMALE, 0, "Beach girl (3)", "Baigneuse (3)" },
	{ "HFOBE", CAT_PEOPLE, G_FEMALE, 0, "Older beachgoer", "Baigneuse âgée" },
	{ "BFOBE", CAT_PEOPLE, G_FEMALE, 0, "Older beachgoer (2)", "Baigneuse âgée (2)" },
	{ "WFOBE", CAT_PEOPLE, G_FEMALE, 0, "Older beachgoer (3)", "Baigneuse âgée (3)" },
	{ "HFYBU", CAT_PEOPLE, G_FEMALE, 0, "Businesswoman", "Femme d'affaires" },
	{ "WFYBU", CAT_PEOPLE, G_FEMALE, 0, "Businesswoman (2)", "Femme d'affaires (2)" },
	{ "HFYMD", CAT_PEOPLE, G_FEMALE, 0, "Maid", "Femme de chambre" },
	{ "HFYPR", CAT_PEOPLE, G_FEMALE, 0, "Prostitute", "Prostituée" },
	{ "BFYPR", CAT_PEOPLE, G_FEMALE, 0, "Prostitute (2)", "Prostituée (2)" },
	{ "WFYPR", CAT_PEOPLE, G_FEMALE, 0, "Prostitute (3)", "Prostituée (3)" },
	{ "BFOTR", CAT_PEOPLE, G_FEMALE, 0, "Bag lady", "Clocharde" },
	{ "WFOTR", CAT_PEOPLE, G_FEMALE, 0, "Bag lady (2)", "Clocharde (2)" },
	{ "WFOGO", CAT_PEOPLE, G_FEMALE, 0, "Golfer", "Golfeuse" },
	{ "WFYLG", CAT_PEOPLE, G_FEMALE, 0, "Lifeguard", "Maître-nageuse" },
	{ "WFYJG", CAT_PEOPLE, G_FEMALE, 0, "Jogger", "Joggeuse" },
	{ "WFYSK", CAT_PEOPLE, G_FEMALE, 0, "Roller skater", "Patineuse" },
	{ "JFOTO", CAT_PEOPLE, G_FEMALE, 0, "Tourist", "Touriste" },
	{ "WFYG1", CAT_PEOPLE, G_FEMALE, 0, "Party girl", "Fêtarde" },
	{ "WFYG2", CAT_PEOPLE, G_FEMALE, 0, "Party girl (2)", "Fêtarde (2)" },
};
enum { NUM_SKINS = ARRAY_SIZE(skins) };

static const char *const catNames[NUM_CATS][2] = {
	{ "Tommy Vercetti", "Tommy Vercetti" },
	{ "Story characters", "Personnages de l'histoire" },
	{ "Police and services", "Police et services" },
	{ "Gangs", "Gangs" },
	{ "Jobs and extras", "Métiers et figurants" },
	{ "Vice City's people", "Habitants de Vice City" },
};

int32
Skins_Index(const char *model)
{
	if (model == nil)
		return -1;
	for (int32 i = 0; i < NUM_SKINS; i++)
		if (strcasecmp(skins[i].model, model) == 0)
			return i;
	return -1;
}

int32 Skins_Count(void) { return NUM_SKINS; }
const char *Skins_Model(int32 i) { return i >= 0 && i < NUM_SKINS ? skins[i].model : "player"; }
bool Skins_Female(int32 i) { return i >= 0 && i < NUM_SKINS && skins[i].gender == G_FEMALE; }

const char *
Lobby_SkinModel(const char *model)
{
	const int32 i = Skins_Index(model);
	return i < 0 ? nil : skins[i].model;
}

bool
Lobby_SkinFemale(const char *model)
{
	return Skins_Female(Skins_Index(model));
}

static const char *
Tr(const char *en, const char *fr)
{
	return LangFrench() ? fr : en;
}

static void
Append(wchar *out, int32 size, const wchar *s)
{
	int32 n = 0;
	while (n < size - 1 && out[n])
		n++;
	for (; s && *s && n < size - 1; s++)
		out[n++] = *s;
	out[n] = 0;
}

// (a skin's name as the game shows it: Tommy's outfits by the game's own texts)
wchar *
Skins_Label(const char *model, wchar *out, int32 size)
{
	const int32 i = Skins_Index(model);
	if (i < 0) {
		GameText(model ? model : "", out, size);
		return out;
	}
	const SkinDef &s = skins[i];
	if (s.outfit == 0) {
		GameText(Tr(s.en, s.fr), out, size);
		return out;
	}
	char key[8];
	wchar part[64];
	snprintf(key, sizeof(key), "OUTFT%d", s.outfit);
	GameText("Tommy (", out, size);
	Append(out, size, TheText.Get(key));
	// (the game names both tracksuits alike)
	GameText(s.outfit == 11 ? " 2)" : ")", part, ARRAY_SIZE(part));
	Append(out, size, part);
	return out;
}

// (a name for sorting: lower case, the Latin-1 accents of UTF-8 folded: an E-acute sorts as "e")
static void
SortKey(const char *utf8, char *out, int32 size)
{
	int32 n = 0;
	for (const uint8 *s = (const uint8 *)utf8; *s && n < size - 1; s++) {
		uint8 c = *s;
		if (c == 0xC3 && s[1]) {
			const uint8 d = s[1] | 0x20; // (the lower case of a Latin-1 letter: 0xA0..0xBF)
			s++;
			c = d >= 0xA0 && d <= 0xA5 ? 'a' : d == 0xA7 ? 'c' : d >= 0xA8 && d <= 0xAB ? 'e' : d >= 0xAC && d <= 0xAF ? 'i' :
			    d >= 0xB2 && d <= 0xB6 ? 'o' : d >= 0xB9 && d <= 0xBC ? 'u' : '?';
		} else if (c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
		out[n++] = c;
	}
	out[n] = '\0';
}

// ---------------------------------------------------------------- the picker's state
enum { F_MALE, F_FEMALE, F_ALL, NUM_FILTERS };
enum { MAX_ROWS = NUM_SKINS + NUM_CATS };
enum { CACHE = 8, LOAD_DELAY_MS = 90 };
enum { P_NONE, P_LOADING, P_READY, P_FAILED };

// (a list row: a skin's index, or a category's heading: -1 - its category)
static struct {
	bool open;
	int32 filter;
	int32 order[NUM_SKINS], orderLang; // (the skins sorted for the language shown: -1 not sorted yet)
	int32 rows[MAX_ROWS], numRows;
	int32 selected;                    // (a skin's index, -1 none)
	int32 first;                       // (the first row shown)
	int32 current;                     // (the character's skin: marked)
	double selectedAt, openedAt;
	// (where the last frame drew things, in pixels, for the mouse)
	struct Place { float left, top, right, bottom; int32 what; } places[64];
	int32 numPlaces;
	float barTop, barBottom, barLeft, barRight;
	int32 visibleRows;
	bool dragging;
} picker = { false, F_ALL, {}, -1 };

// What a click is on.
enum { W_ROW = 0, W_TAB = 0x1000, W_CHOOSE = 0x2000, W_BACK = 0x2001 };

static bool
RowIsSkin(int32 r)
{
	return r >= 0 && r < picker.numRows && picker.rows[r] >= 0;
}

static void
SortSkins(void)
{
	const int32 lang = LangFrench() ? 1 : 0;
	if (picker.orderLang == lang)
		return;
	static char keys[NUM_SKINS][48];
	for (int32 i = 0; i < NUM_SKINS; i++) {
		picker.order[i] = i;
		SortKey(skins[i].outfit ? "" : Tr(skins[i].en, skins[i].fr), keys[i], sizeof(keys[i]));
	}
	// (by category, Tommy's outfits in the game's order, the others by name: an insertion sort, a few hundred)
	for (int32 i = 1; i < NUM_SKINS; i++) {
		const int32 v = picker.order[i];
		int32 j = i - 1;
		for (; j >= 0; j--) {
			const SkinDef &a = skins[picker.order[j]], &b = skins[v];
			int32 c = a.cat - b.cat;
			if (c == 0)
				c = a.outfit || b.outfit ? a.outfit - b.outfit : strcmp(keys[picker.order[j]], keys[v]);
			if (c <= 0)
				break;
			picker.order[j + 1] = picker.order[j];
		}
		picker.order[j + 1] = v;
	}
	picker.orderLang = lang;
}

static bool
InFilter(int32 i)
{
	return picker.filter == F_ALL || (picker.filter == F_FEMALE) == (skins[i].gender == G_FEMALE);
}

static int32
RowOf(int32 skin)
{
	for (int32 r = 0; r < picker.numRows; r++)
		if (picker.rows[r] == skin)
			return r;
	return -1;
}

static void
BuildRows(void)
{
	SortSkins();
	picker.numRows = 0;
	int32 cat = -1;
	for (int32 k = 0; k < NUM_SKINS; k++) {
		const int32 i = picker.order[k];
		if (!InFilter(i))
			continue;
		if (skins[i].cat != cat) {
			cat = skins[i].cat;
			picker.rows[picker.numRows++] = -1 - cat;
		}
		picker.rows[picker.numRows++] = i;
	}
}

// (the list scrolled so that `row` is shown with two rows around it, its category's heading when it is the
// category's first)
static void
ShowRow(int32 row)
{
	const int32 visible = Max(1, picker.visibleRows), margin = visible > 8 ? 2 : 0;
	if (row < 0)
		return;
	int32 top = row - margin;
	if (row > 0 && !RowIsSkin(row - 1))
		top = Min(top, row - 1);
	if (top < picker.first)
		picker.first = top;
	else if (row + margin >= picker.first + visible)
		picker.first = row + margin - visible + 1;
	picker.first = Clamp(picker.first, 0, Max(0, picker.numRows - visible));
}

static void
Select(int32 skin)
{
	if (skin == picker.selected)
		return;
	picker.selected = skin;
	picker.selectedAt = Now();
}

static void
SelectRow(int32 row, bool show = true)
{
	if (!RowIsSkin(row))
		return;
	Select(picker.rows[row]);
	if (show)
		ShowRow(row);
}

static void
SetFilter(int32 filter)
{
	picker.filter = (filter + NUM_FILTERS) % NUM_FILTERS;
	BuildRows();
	picker.first = 0;
	int32 row = RowOf(picker.selected);
	if (row < 0) {
		// (the selection not in this list: its first skin)
		for (row = 0; row < picker.numRows && !RowIsSkin(row); row++)
			;
		if (row >= picker.numRows)
			row = -1;
	}
	if (row >= 0) {
		SelectRow(row, false);
		// (the selection placed a third down the list, as when the page opens)
		picker.first = Clamp(row - picker.visibleRows / 3, 0, Max(0, picker.numRows - picker.visibleRows));
		ShowRow(row);
	}
}

// (a step through the skins of the list, headings skipped; at an end, it stays)
static void
Step(int32 delta)
{
	int32 row = RowOf(picker.selected);
	if (row < 0) {
		SetFilter(picker.filter);
		return;
	}
	const int32 dir = delta > 0 ? 1 : -1;
	for (int32 n = delta > 0 ? delta : -delta; n > 0; n--) {
		int32 next = row + dir;
		while (next >= 0 && next < picker.numRows && !RowIsSkin(next))
			next += dir;
		if (next < 0 || next >= picker.numRows)
			break;
		row = next;
	}
	SelectRow(row);
}

// ---------------------------------------------------------------- the game's files, read without blocking a frame
// (a file of the game open for reading, its case as the game's files have it; -1 if none)
static int
OpenGameFile(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		char *real = casepath(path, false);
		if (real) {
			fd = open(real, O_RDONLY);
			free(real);
		}
	}
	return fd;
}

// (its bytes [position, position + length) here: else the page fetches them meanwhile)
static bool
BytesReady(int fd, uint32 position, uint32 length)
{
#ifdef __EMSCRIPTEN__
	return OrthrosFileReady(fd, (double)position, (int)length) != 0;
#else
	return true;
#endif
}

static bool
ReadAt(int fd, uint32 position, void *buffer, uint32 length)
{
	if (lseek(fd, position, SEEK_SET) != (off_t)position)
		return false;
	uint32 done = 0;
	while (done < length) {
		const ssize_t n = read(fd, (char *)buffer + done, length - done);
		if (n <= 0)
			return false;
		done += (uint32)n;
	}
	return true;
}

// ---------------------------------------------------------------- the idle stances (anim/ped.ifp)
// One animation: its sequences' keyframes, by bone tag (the hierarchy's node ids), rotations as the game keeps them
// (the file's inverted), as CAnimManager::LoadAnimFile reads them.
enum { MAX_SEQS = 32 };
struct AnimKey { float time; CQuaternion q; CVector t; };
struct AnimSeq { int32 tag; bool translation; int32 numKeys; AnimKey *keys; };
struct IdleAnim { int32 numSeqs; AnimSeq seqs[MAX_SEQS]; float duration; };

static void
FreeAnim(IdleAnim &a)
{
	for (int32 i = 0; i < a.numSeqs; i++)
		delete[] a.seqs[i].keys;
	a.numSeqs = 0;
	a.duration = 0.0f;
}

// (the animation named `name` of an IFP's bytes, false if not there; ANPK, INFO, then NAME + DGAN per animation)
static bool
ParseIfpAnim(const uint8 *data, uint32 size, const char *name, IdleAnim &out)
{
#define IFP_ROUND(x) (((x) + 3) & ~3u)
	struct Header { char ident[4]; uint32 size; };
	uint32 p = 0;
	auto header = [&](Header &h) -> bool {
		if (p + 8 > size)
			return false;
		memcpy(&h, data + p, 8);
		p += 8;
		return true;
	};
	Header h;
	if (!header(h) || strncmp(h.ident, "ANPK", 4) != 0 || !header(h) || p + 4 > size)
		return false;
	int32 numAnims;
	memcpy(&numAnims, data + p, 4);
	p += IFP_ROUND(h.size);
	for (int32 a = 0; a < numAnims; a++) {
		if (!header(h) || p + h.size > size)
			return false;
		char animName[32];
		snprintf(animName, sizeof(animName), "%.*s", (int)Min(h.size, 31u), (const char *)data + p);
		p += IFP_ROUND(h.size);
		Header dgan;
		if (!header(dgan))
			return false;
		const uint32 next = p + IFP_ROUND(dgan.size);
		if (strcasecmp(animName, name) != 0) {
			p = next;
			continue;
		}
		Header info;
		if (!header(info) || p + 4 > size)
			return false;
		int32 numSeqs;
		memcpy(&numSeqs, data + p, 4);
		p += IFP_ROUND(info.size);
		FreeAnim(out);
		for (int32 s = 0; s < numSeqs && p < next; s++) {
			Header cpan, anim;
			if (!header(cpan))
				return false;
			const uint32 after = p + IFP_ROUND(cpan.size);
			if (!header(anim) || p + anim.size > size)
				return false;
			int32 numFrames = 0, tag = -1;
			memcpy(&numFrames, data + p + 28, 4);
			if (anim.size == 44)
				memcpy(&tag, data + p + 40, 4);
			p += IFP_ROUND(anim.size);
			Header kind;
			if (numFrames > 0 && tag >= 0 && out.numSeqs < MAX_SEQS && header(kind)) {
				const bool scale = strncmp(kind.ident, "KRTS", 4) == 0, trans = scale || strncmp(kind.ident, "KRT0", 4) == 0;
				const uint32 stride = scale ? 0x2C : trans ? 0x20 : 0x14;
				if (p + stride * numFrames <= size) {
					AnimSeq &seq = out.seqs[out.numSeqs++];
					seq.tag = tag;
					seq.translation = trans;
					seq.numKeys = numFrames;
					seq.keys = new AnimKey[numFrames];
					for (int32 k = 0; k < numFrames; k++) {
						float f[11];
						memcpy(f, data + p + k * stride, stride);
						AnimKey &key = seq.keys[k];
						key.q = CQuaternion(f[0], f[1], f[2], f[3]);
						key.q.Invert();
						key.t = trans ? CVector(f[4], f[5], f[6]) : CVector(0.0f, 0.0f, 0.0f);
						key.time = f[scale ? 10 : trans ? 7 : 4];
						out.duration = Max(out.duration, key.time);
					}
				}
			}
			p = after;
		}
		return out.numSeqs > 0 && out.duration > 0.0f;
	}
	return false;
#undef IFP_ROUND
}

// (a sequence at `time`: its rotation and translation between the keys around it)
static void
SampleSeq(const AnimSeq &seq, float time, CQuaternion &q, CVector &t)
{
	int32 k = 0;
	while (k + 1 < seq.numKeys && seq.keys[k + 1].time <= time)
		k++;
	const AnimKey &a = seq.keys[k], &b = seq.keys[Min(k + 1, seq.numKeys - 1)];
	const float span = b.time - a.time, f = span > 0.0f ? Clamp((time - a.time) / span, 0.0f, 1.0f) : 0.0f;
	// (a normalised lerp, the shorter way: the keys are close)
	const float sign = DotProduct(a.q, b.q) < 0.0f ? -1.0f : 1.0f;
	q = CQuaternion(a.q.x + (sign * b.q.x - a.q.x) * f, a.q.y + (sign * b.q.y - a.q.y) * f, a.q.z + (sign * b.q.z - a.q.z) * f,
		a.q.w + (sign * b.q.w - a.q.w) * f);
	q.Normalise();
	t = a.t + (b.t - a.t) * f;
}

// ---------------------------------------------------------------- the preview's models
struct DirEntry { uint32 offset, size; char name[24]; };
struct Model {
	int32 skin, state;
	uint32 dff[2], txd[2];            // (their places in the image: offset and size in bytes)
	RpClump *clump;
	RwTexDictionary *txdict;
	RpHAnimAnimation *hanim;          // (the hierarchy's interpolator needs one, as CClumpModelInfo::CreateInstance)
	RpHAnimHierarchy *hier;
	CQuaternion *bindQ; CVector *bindT; // (each bone's local rotation and place at rest: the bones no stance moves)
	uint32 used;
};

static struct {
	int imgFd, dirFd, ifpFd;
	int32 dirState, animState;
	DirEntry *dir; int32 dirCount;
	IdleAnim idle[2];                 // (men's, women's)
	Model models[CACHE];
	uint32 clock;
	float angle, heldAngle;
	bool held;                        // (the turn stopped at heldAngle: "mpturn")
	double lastTurn;
} preview = { -1, -1, -1, P_NONE, P_NONE, nil, 0 };

static const DirEntry *
FindEntry(const char *name)
{
	for (int32 i = 0; i < preview.dirCount; i++)
		if (strcasecmp(preview.dir[i].name, name) == 0)
			return &preview.dir[i];
	return nil;
}

static void
FreeModel(Model &m)
{
	if (m.clump)
		RpClumpDestroy(m.clump);
	if (m.hanim)
		RpHAnimAnimationDestroy(m.hanim);
	if (m.txdict)
		RwTexDictionaryDestroy(m.txdict);
	delete[] m.bindQ;
	delete[] m.bindT;
	memset(&m, 0, sizeof(m));
	m.skin = -1;
	m.state = P_NONE;
}

static void
FreePreview(void)
{
	for (int32 i = 0; i < CACHE; i++)
		FreeModel(preview.models[i]);
	FreeAnim(preview.idle[0]);
	FreeAnim(preview.idle[1]);
	delete[] preview.dir;
	preview.dir = nil;
	preview.dirCount = 0;
	if (preview.imgFd >= 0) close(preview.imgFd);
	if (preview.dirFd >= 0) close(preview.dirFd);
	if (preview.ifpFd >= 0) close(preview.ifpFd);
	preview.imgFd = preview.dirFd = preview.ifpFd = -1;
	preview.dirState = preview.animState = P_NONE;
}

// (a whole small file of the game read once its bytes are here: P_LOADING until then)
static int32
PollWholeFile(int &fd, const char *path, int32 state, uint8 **data, uint32 *size)
{
	if (state == P_READY || state == P_FAILED)
		return state;
	if (fd < 0 && (fd = OpenGameFile(path)) < 0)
		return P_FAILED;
	const off_t end = lseek(fd, 0, SEEK_END);
	if (end <= 0 || end > 16 * 1024 * 1024)
		return P_FAILED;
	if (!BytesReady(fd, 0, (uint32)end))
		return P_LOADING;
	*data = new uint8[end];
	*size = (uint32)end;
	if (!ReadAt(fd, 0, *data, (uint32)end)) {
		delete[] *data;
		*data = nil;
		return P_FAILED;
	}
	return P_READY;
}

static void
PollDirectory(void)
{
	uint8 *data = nil;
	uint32 size = 0;
	const int32 was = preview.dirState;
	preview.dirState = PollWholeFile(preview.dirFd, "models/gta3.dir", was, &data, &size);
	if (data) {
		preview.dirCount = size / 32;
		preview.dir = new DirEntry[preview.dirCount];
		for (int32 i = 0; i < preview.dirCount; i++) {
			memcpy(&preview.dir[i].offset, data + i * 32, 4);
			memcpy(&preview.dir[i].size, data + i * 32 + 4, 4);
			memcpy(preview.dir[i].name, data + i * 32 + 8, 24);
			preview.dir[i].name[23] = '\0';
		}
		delete[] data;
		close(preview.dirFd);
		preview.dirFd = -1;
	}
	if (preview.dirState == P_READY && preview.imgFd < 0 && (preview.imgFd = OpenGameFile("models/gta3.img")) < 0)
		preview.dirState = P_FAILED;
}

static void
PollAnims(void)
{
	uint8 *data = nil;
	uint32 size = 0;
	preview.animState = PollWholeFile(preview.ifpFd, "anim/ped.ifp", preview.animState, &data, &size);
	if (data) {
		// (the stances the game's peds stand in: a man's, a woman's)
		if (!ParseIfpAnim(data, size, "idle_stance", preview.idle[0]))
			preview.animState = P_FAILED;
		else if (!ParseIfpAnim(data, size, "woman_idlestance", preview.idle[1]))
			FreeAnim(preview.idle[1]);
		delete[] data;
		close(preview.ifpFd);
		preview.ifpFd = -1;
	}
}

static RpAtomic *
SetSkinHierarchy(RpAtomic *atomic, void *data)
{
	RpSkinAtomicSetHAnimHierarchy(atomic, (RpHAnimHierarchy *)data);
	return atomic;
}

// (a rotation matrix's quaternion, as librw's Matrix::makeRotation reads it back)
static CQuaternion
MatrixQuat(const RwMatrix &m)
{
	CQuaternion q;
	const float trace = m.right.x + m.up.y + m.at.z;
	if (trace > 0.0f) {
		const float s = Sqrt(trace + 1.0f) * 2.0f;
		q = CQuaternion((m.up.z - m.at.y) / s, (m.at.x - m.right.z) / s, (m.right.y - m.up.x) / s, 0.25f * s);
	} else if (m.right.x > m.up.y && m.right.x > m.at.z) {
		const float s = Sqrt(1.0f + m.right.x - m.up.y - m.at.z) * 2.0f;
		q = CQuaternion(0.25f * s, (m.up.x + m.right.y) / s, (m.at.x + m.right.z) / s, (m.up.z - m.at.y) / s);
	} else if (m.up.y > m.at.z) {
		const float s = Sqrt(1.0f + m.up.y - m.right.x - m.at.z) * 2.0f;
		q = CQuaternion((m.up.x + m.right.y) / s, 0.25f * s, (m.at.y + m.up.z) / s, (m.at.x - m.right.z) / s);
	} else {
		const float s = Sqrt(1.0f + m.at.z - m.right.x - m.up.y) * 2.0f;
		q = CQuaternion((m.at.x + m.right.z) / s, (m.at.y + m.up.z) / s, 0.25f * s, (m.right.y - m.up.x) / s);
	}
	q.Normalise();
	return q;
}

// (the model's bones at rest, from its skin's matrices: each bone's place in its parent's, as SkinGetBonePositionsToTable
// walks them)
static void
BindPose(Model &m)
{
	RpAtomic *atomic = GetFirstAtomic(m.clump);
	RpSkin *skin = RpSkinGeometryGetSkin(RpAtomicGetGeometry(atomic));
	const int32 n = m.hier->numNodes;
	const RwMatrix *toBone = RpSkinGetSkinToBoneMatrices(skin);
	m.bindQ = new CQuaternion[n];
	m.bindT = new CVector[n];
	int32 stack[64], sp = 0, parent = -1;
	for (int32 i = 0; i < n; i++) {
		RwMatrix boneToSkin, local;
		RwMatrixInvert(&boneToSkin, &toBone[i]);
		if (parent >= 0)
			rw::Matrix::mult(&local, &boneToSkin, &toBone[parent]); // (fakerw has no RwMatrixMultiply)
		else
			local = boneToSkin;
		m.bindQ[i] = MatrixQuat(local);
		m.bindT[i] = CVector(local.pos.x, local.pos.y, local.pos.z);
		const uint32 flags = HIERNODEINFO(m.hier)[i].flags;
		if (flags & rpHANIMPUSHPARENTMATRIX)
			stack[sp < 63 ? ++sp : sp] = parent;
		if (flags & rpHANIMPOPPARENTMATRIX)
			parent = sp > 0 ? stack[sp--] : -1;
		else
			parent = i;
	}
}

// (the model of a skin from its DFF and TXD bytes; false if they are not a skinned ped's)
static bool
BuildModel(Model &m, uint8 *dff, uint8 *txd)
{
	RwMemory mem;
	mem.start = txd;
	mem.length = m.txd[1];
	RwStream *stream = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &mem);
	if (stream) {
		if (RwStreamFindChunk(stream, rwID_TEXDICTIONARY, nil, nil))
			m.txdict = RwTexDictionaryGtaStreamRead(stream);
		RwStreamClose(stream, &mem);
	}
	if (m.txdict == nil)
		return false;
	RwTexDictionary *before = RwTexDictionaryGetCurrent();
	RwTexDictionarySetCurrent(m.txdict);
	mem.start = dff;
	mem.length = m.dff[1];
	stream = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &mem);
	if (stream) {
		if (RwStreamFindChunk(stream, rwID_CLUMP, nil, nil))
			m.clump = RpClumpStreamRead(stream);
		RwStreamClose(stream, &mem);
	}
	RwTexDictionarySetCurrent(before);
	if (m.clump == nil || !IsClumpSkinned(m.clump) || (m.hier = GetAnimHierarchyFromClump(m.clump)) == nil)
		return false;
	// (as CClumpModelInfo::SetClump and CreateInstance: the skin on the hierarchy, its weights summed to one, the
	// hierarchy's interpolator given an animation)
	RpClumpForAllAtomics(m.clump, SetSkinHierarchy, m.hier);
	RpAtomic *atomic = GetFirstAtomic(m.clump);
	RpGeometry *geometry = RpAtomicGetGeometry(atomic);
	RpSkin *skin = RpSkinGeometryGetSkin(geometry);
	if (skin == nil)
		return false;
	for (int32 i = 0; i < RpGeometryGetNumVertices(geometry); i++) {
		RwMatrixWeights *w = (RwMatrixWeights *)&RpSkinGetVertexBoneWeights(skin)[i];
		const float sum = w->w0 + w->w1 + w->w2 + w->w3;
		if (sum > 0.0f) {
			w->w0 /= sum;
			w->w1 /= sum;
			w->w2 /= sum;
			w->w3 /= sum;
		}
	}
	m.hanim = HAnimAnimationCreateForHierarchy(m.hier);
	if (m.hanim == nil)
		return false;
	RpHAnimHierarchySetCurrentAnim(m.hier, m.hanim);
	RpHAnimHierarchySetFlags(m.hier, (RpHAnimHierarchyFlag)(rpHANIMHIERARCHYUPDATEMODELLINGMATRICES | rpHANIMHIERARCHYUPDATELTMS));
	BindPose(m);
	return true;
}

static Model *
CachedModel(int32 skin)
{
	for (int32 i = 0; i < CACHE; i++)
		if (preview.models[i].state != P_NONE && preview.models[i].skin == skin)
			return &preview.models[i];
	return nil;
}

// (a slot for a new model: a free one, or the least recently used, never the one shown)
static Model *
FreeSlot(int32 keep)
{
	Model *best = nil;
	for (int32 i = 0; i < CACHE; i++) {
		Model &m = preview.models[i];
		if (m.state == P_NONE)
			return &m;
		if (m.skin != keep && (best == nil || m.used < best->used))
			best = &m;
	}
	if (best)
		FreeModel(*best);
	return best;
}

// (the image's place of a skin's DFF and TXD, false if it has none)
static bool
Locate(int32 skin, uint32 dff[2], uint32 txd[2])
{
	char name[32];
	snprintf(name, sizeof(name), "%s.dff", skins[skin].model);
	const DirEntry *d = FindEntry(name);
	snprintf(name, sizeof(name), "%s.txd", skins[skin].model);
	const DirEntry *t = FindEntry(name);
	if (d == nil || t == nil)
		return false;
	dff[0] = d->offset * 2048;
	dff[1] = d->size * 2048;
	txd[0] = t->offset * 2048;
	txd[1] = t->size * 2048;
	return true;
}

// (a skin's bytes asked of the page ahead: its neighbours')
static void
Prefetch(int32 skin)
{
	uint32 dff[2], txd[2];
	if (preview.imgFd >= 0 && skin >= 0 && CachedModel(skin) == nil && Locate(skin, dff, txd)) {
		BytesReady(preview.imgFd, dff[0], dff[1]);
		BytesReady(preview.imgFd, txd[0], txd[1]);
	}
}

static void
PollModel(Model &m)
{
	if (m.state != P_LOADING)
		return;
	if (!BytesReady(preview.imgFd, m.dff[0], m.dff[1]) || !BytesReady(preview.imgFd, m.txd[0], m.txd[1]))
		return;
	uint8 *dff = (uint8 *)RwMallocAlign(m.dff[1], 2048), *txd = (uint8 *)RwMallocAlign(m.txd[1], 2048);
	const bool read = dff && txd && ReadAt(preview.imgFd, m.dff[0], dff, m.dff[1]) && ReadAt(preview.imgFd, m.txd[0], txd, m.txd[1]);
	const int32 skin = m.skin;
	const bool built = read && BuildModel(m, dff, txd);
	if (dff) RwFreeAlign(dff);
	if (txd) RwFreeAlign(txd);
	if (!built) {
		debug("skins: %s could not be read\n", skins[skin].model);
		FreeModel(m);
		m.skin = skin;
		m.state = P_FAILED;
		return;
	}
	m.state = P_READY;
}

// (the selected skin's model asked for once the selection rested a moment (scrolling through the list reads nothing),
// or shown at once when it is kept)
static void
PollPreview(void)
{
	PollDirectory();
	PollAnims();
	for (int32 i = 0; i < CACHE; i++)
		if (preview.models[i].state == P_LOADING)
			PollModel(preview.models[i]);
	const int32 skin = picker.selected;
	if (skin < 0 || preview.dirState != P_READY)
		return;
	Model *m = CachedModel(skin);
	if (m == nil && Now() - picker.selectedAt >= LOAD_DELAY_MS) {
		m = FreeSlot(skin);
		if (m == nil)
			return;
		m->skin = skin;
		m->state = P_LOADING;
		if (!Locate(skin, m->dff, m->txd))
			m->state = P_FAILED;
		else
			PollModel(*m);
	}
	if (m)
		m->used = ++preview.clock;
	// (shown: the next and previous ones asked ahead)
	if (m && m->state == P_READY) {
		const int32 row = RowOf(skin);
		for (int32 d = -2; d <= 2; d++)
			if (d != 0 && RowIsSkin(row + d))
				Prefetch(picker.rows[row + d]);
	}
}

static int32
PreviewState(void)
{
	const Model *m = picker.selected >= 0 ? CachedModel(picker.selected) : nil;
	if (preview.dirState == P_FAILED)
		return P_FAILED;
	return m ? m->state : picker.selected >= 0 ? P_LOADING : P_NONE;
}

// (the model's bones in its stance at `time`: the stance's sequences, the bind pose for the bones it leaves)
static void
Pose(Model &m, const IdleAnim *anim, float time)
{
	const int32 n = m.hier->numNodes;
	for (int32 i = 0; i < n; i++) {
		RpHAnimStdInterpFrame *f = (RpHAnimStdInterpFrame *)rpHANIMHIERARCHYGETINTERPFRAME(m.hier, i);
		CQuaternion q = m.bindQ[i];
		CVector t = m.bindT[i];
		const int32 tag = HIERNODEID(m.hier, i);
		for (int32 s = 0; anim && s < anim->numSeqs; s++)
			if (anim->seqs[s].tag == tag) {
				CVector at;
				SampleSeq(anim->seqs[s], time, q, at);
				if (anim->seqs[s].translation)
					t = i == 0 ? CVector(0.0f, 0.0f, at.z) : at; // (the root's sway in place, as the game extracts it)
				break;
			}
		f->q.imag.x = q.x;
		f->q.imag.y = q.y;
		f->q.imag.z = q.z;
		f->q.real = q.w;
		f->t.x = t.x;
		f->t.y = t.y;
		f->t.z = t.z;
	}
}

// (the selected skin drawn: its feet at the screen's (x, y) in pixels, `height` pixels tall, turned `angle` degrees
// from facing the camera, in its idle stance, lit for the menu and the lights put back after)
static void
RenderModel(Model &m, float x, float y, float height, float angle)
{
	RwCamera *camera = Scene.camera;
	RwFrame *cameraFrame = RwCameraGetFrame(camera);
	if (camera == nil || cameraFrame == nil)
		return;
	const RwMatrix *cam = RwFrameGetLTM(cameraFrame);
	const RwV2d *vw = RwCameraGetViewWindow(camera);
	// (a ped is about 1.9 m tall, its frame's origin about 1 m above its feet)
	const float PED_HEIGHT = 1.9f, ORIGIN_UP = 1.0f;
	const float fraction = height / SCREEN_HEIGHT;
	const float distance = PED_HEIGHT / (2.0f * vw->y * fraction);
	const float nx = x / SCREEN_WIDTH * 2.0f - 1.0f, ny = 1.0f - y / SCREEN_HEIGHT * 2.0f;
	// (the feet's point on the screen, then the frame's origin above them)
	CVector right(cam->right.x, cam->right.y, cam->right.z), up(cam->up.x, cam->up.y, cam->up.z), at(cam->at.x, cam->at.y, cam->at.z);
	CVector pos(cam->pos.x, cam->pos.y, cam->pos.z);
	pos += at * distance - right * (nx * vw->x * distance) + up * (ny * vw->y * distance + ORIGIN_UP);
	// (standing up the screen, facing the camera, turned)
	const float a = DEGTORAD(angle);
	const CVector face = -at * Cos(a) + right * Sin(a);
	const CVector side = CrossProduct(face, up);
	RwFrame *frame = RpClumpGetFrame(m.clump);
	RwMatrix *mat = RwFrameGetMatrix(frame);
	mat->right.x = side.x; mat->right.y = side.y; mat->right.z = side.z;
	mat->up.x = face.x; mat->up.y = face.y; mat->up.z = face.z;
	mat->at.x = up.x; mat->at.y = up.y; mat->at.z = up.z;
	mat->pos.x = pos.x; mat->pos.y = pos.y; mat->pos.z = pos.z;
	RwMatrixUpdate(mat);
	RwFrameUpdateObjects(frame);

	const bool female = skins[m.skin].gender == G_FEMALE && preview.idle[1].numSeqs > 0;
	const IdleAnim *anim = preview.animState == P_READY ? &preview.idle[female ? 1 : 0] : nil;
	const float time = anim ? (float)fmod(CTimer::GetTimeInMillisecondsPauseMode() / 1000.0, (double)anim->duration) : 0.0f;
	Pose(m, anim, time);
	RpHAnimHierarchyUpdateMatrices(m.hier);

	// (the light: the menus' ambient a little brighter, the sun from the viewer's upper left; put back after)
	// (librw's lights directly: fakerw has no RpLightGetColor / RpLightGetFlags)
	RwRGBAReal ambientWas = pAmbient->color, directWas = pDirect->color;
	RwFrame *lightFrame = RpLightGetFrame(pDirect);
	RwMatrix lightWas = *RwFrameGetMatrix(lightFrame);
	const RwRGBAReal ambient = { 0.55f, 0.55f, 0.55f, 1.0f }, direct = { 0.85f, 0.82f, 0.75f, 1.0f };
	RpLightSetColor(pAmbient, &ambient);
	RpLightSetColor(pDirect, &direct);
	CVector dir = at * 1.0f - up * 0.7f - right * 0.5f;
	dir.Normalise();
	CVector lr = CrossProduct(dir, up);
	lr.Normalise();
	CVector lu = CrossProduct(lr, dir);
	RwMatrix *lm = RwFrameGetMatrix(lightFrame);
	lm->right.x = lr.x; lm->right.y = lr.y; lm->right.z = lr.z;
	lm->up.x = lu.x; lm->up.y = lu.y; lm->up.z = lu.z;
	lm->at.x = dir.x; lm->at.y = dir.y; lm->at.z = dir.z;
	RwMatrixUpdate(lm);
	RwFrameUpdateObjects(lightFrame);
	const uint32 directFlags = pDirect->getFlags();
	RpLightSetFlags(pDirect, rpLIGHTLIGHTATOMICS);

	DefinedState();
	RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void *)TRUE);
	RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void *)TRUE);
	RpClumpRender(m.clump);

	RpLightSetFlags(pDirect, directFlags);
	*lm = lightWas;
	RwMatrixUpdate(lm);
	RwFrameUpdateObjects(lightFrame);
	RpLightSetColor(pAmbient, &ambientWas);
	RpLightSetColor(pDirect, &directWas);
	FrontEndMenuManager.SetFrontEndRenderStates();
}

// ---------------------------------------------------------------- opening and closing
static void
PickerFree(void)
{
	FreePreview();
	picker.numRows = 0;
	picker.selected = -1;
	picker.dragging = false;
}

void
Skins_PickerOpen(const char *current)
{
	PickerFree();
	for (int32 i = 0; i < CACHE; i++) {
		preview.models[i].skin = -1;
		preview.models[i].state = P_NONE;
	}
	picker.open = true;
	picker.openedAt = Now();
	picker.current = Skins_Index(current);
	picker.selected = -1;
	picker.visibleRows = 20;
	// (the list of the current skin's gender (a woman's skin: the women's), the current skin selected)
	picker.filter = picker.current >= 0 && skins[picker.current].gender == G_FEMALE ? F_FEMALE : F_MALE;
	picker.selected = picker.current >= 0 ? picker.current : 0;
	SetFilter(picker.filter);
	picker.selectedAt = Now() - LOAD_DELAY_MS;
}

void
Skins_PickerClose(void)
{
	if (!picker.open)
		return;
	picker.open = false;
	PickerFree();
}

bool
Skins_PickerOpenNow(void)
{
	return picker.open;
}

static void
Choose(void)
{
	if (picker.selected >= 0)
		Lobby_SkinPicked(skins[picker.selected].model);
}

static void
Cancel(void)
{
	Lobby_SkinPicked(nil);
}

// ---------------------------------------------------------------- input
bool
Skins_PickerKey(int32 key)
{
	if (!picker.open)
		return false;
	switch (key) {
	case rsUP: case rsPADUP: Step(-1); break;
	case rsDOWN: case rsPADDOWN: Step(1); break;
	case rsPGUP: case rsPADPGUP: Step(-Max(1, picker.visibleRows - 2)); break;
	case rsPGDN: case rsPADPGDN: Step(Max(1, picker.visibleRows - 2)); break;
	case rsHOME: case rsPADHOME: Step(-MAX_ROWS); break;
	case rsEND: case rsPADEND: Step(MAX_ROWS); break;
	case rsLEFT: case rsPADLEFT: SetFilter(picker.filter - 1); break;
	case rsRIGHT: case rsPADRIGHT: case rsTAB: SetFilter(picker.filter + 1); break;
	case rsENTER: case rsPADENTER: Choose(); break;
	case rsESC: case rsBACKSP: Cancel(); break;
	default: break;
	}
	// (every key the picker's while it is open: the menu is frozen meanwhile)
	return true;
}

// (the mouse: the wheel scrolls, a click selects a row (on the selected one, chooses it), a tab filters, the
// scrollbar drags, the buttons choose or go back)
static void
PickerMouse(void)
{
	CMenuManager &mm = FrontEndMenuManager;
	CPad *pad = CPad::GetPad(0);
	const float mx = mm.m_nMousePosX, my = mm.m_nMousePosY;
	const int32 visible = Max(1, picker.visibleRows), maxFirst = Max(0, picker.numRows - visible);
	if (pad->GetMouseWheelUpJustDown())
		picker.first = Max(0, picker.first - 3);
	else if (pad->GetMouseWheelDownJustDown())
		picker.first = Min(maxFirst, picker.first + 3);
	if (picker.dragging) {
		if (!pad->GetLeftMouse()) {
			picker.dragging = false;
		} else if (picker.barBottom > picker.barTop) {
			const float f = Clamp((my - picker.barTop) / (picker.barBottom - picker.barTop), 0.0f, 1.0f);
			picker.first = Clamp((int32)(f * picker.numRows - visible / 2), 0, maxFirst);
		}
		return;
	}
	if (!pad->GetLeftMouseJustDown())
		return;
	if (mx >= picker.barLeft && mx <= picker.barRight && my >= picker.barTop && my <= picker.barBottom && picker.numRows > visible) {
		picker.dragging = true;
		return;
	}
	for (int32 i = 0; i < picker.numPlaces; i++) {
		const auto &p = picker.places[i];
		if (mx < p.left || mx > p.right || my < p.top || my > p.bottom)
			continue;
		if (p.what >= W_CHOOSE) {
			if (p.what == W_CHOOSE)
				Choose();
			else
				Cancel();
		} else if (p.what >= W_TAB) {
			if (picker.filter != p.what - W_TAB)
				SetFilter(p.what - W_TAB);
		} else if (RowIsSkin(p.what)) {
			if (picker.rows[p.what] == picker.selected)
				Choose();
			else
				SelectRow(p.what, false);
		}
		return;
	}
}

void
Skins_PickerFrame(void)
{
	if (!picker.open)
		return;
	CMenuManager &mm = FrontEndMenuManager;
	if (mm.m_nCurrScreen != MENUPAGE_MMO_SKINS)
		return;
	// (the menu's own navigation frozen while the page is shown, as the pseudo's text field does)
	PickerMouse();
	if (picker.open)
		PollPreview();
}

// ---------------------------------------------------------------- drawing
// (in the menus' 640 x 448 units, kept in proportion and centred as the menus' pages are)
static float X(float x) { return SCALE_AND_CENTER_X(x); }
static float Y(float y) { return SCREEN_SCALE_Y(y); }
static float W(float w) { return SCREEN_SCALE_X(w); }

static const CRGBA PINK(255, 150, 225, 255), CYAN(97, 194, 247, 255), WHITE(255, 255, 255, 255), GREY(170, 170, 170, 255),
	GREEN(25, 130, 70, 255), PANEL(49, 101, 148, 130), BLACK(0, 0, 0, 255);

static CRGBA
Faded(const CRGBA &c, int32 alpha)
{
	return CRGBA(c.r, c.g, c.b, (uint8)(c.a * alpha / 255));
}

static void
AddPlace(float left, float top, float right, float bottom, int32 what)
{
	if (picker.numPlaces >= (int32)ARRAY_SIZE(picker.places))
		return;
	picker.places[picker.numPlaces++] = { left, top, right, bottom, what };
}

static bool
Hovered(float left, float top, float right, float bottom)
{
	const float mx = FrontEndMenuManager.m_nMousePosX, my = FrontEndMenuManager.m_nMousePosY;
	return FrontEndMenuManager.m_bShowMouse && mx >= left && mx <= right && my >= top && my <= bottom;
}

// Layout, menu units: the preview on the left under the game's logo, the filter and the list on the right.
static const float LIST_L = 262.0f, LIST_R = 606.0f, LIST_T = 90.0f, LIST_B = 384.0f, ROW_H = 14.0f, TABS_Y = 62.0f;
static const float VIEW_L = 34.0f, VIEW_R = 250.0f, VIEW_T = 146.0f, VIEW_B = 384.0f, BUTTONS_Y = 394.0f;

static void
DrawTabs(int32 alpha)
{
	static const char *const names[NUM_FILTERS][2] = { { "Men", "Hommes" }, { "Women", "Femmes" }, { "All", "Tous" } };
	const int32 lang = LangFrench() ? 1 : 0;
	wchar text[32];
	CFont::SetFontStyle(FONT_LOCALE(FONT_HEADING));
	CFont::SetScale(W(0.5f), Y(0.9f));
	CFont::SetRightJustifyOff();
	CFont::SetCentreOff();
	CFont::SetDropShadowPosition(2);
	CFont::SetDropColor(Faded(BLACK, alpha));
	float x = LIST_L + 4.0f;
	for (int32 f = 0; f < NUM_FILTERS; f++) {
		GameText(names[f][lang], text, ARRAY_SIZE(text));
		const float width = CFont::GetStringWidth(text, true);
		const float left = X(x) - W(7.0f), right = X(x) + width + W(7.0f), top = Y(TABS_Y - 2.0f), bottom = Y(TABS_Y + 20.0f);
		const bool active = f == picker.filter;
		if (active)
			CSprite2d::DrawRect(CRect(left, top, right, bottom), Faded(GREEN, alpha));
		CFont::SetColor(Faded(active || Hovered(left, top, right, bottom) ? WHITE : PINK, alpha));
		CFont::PrintString(X(x), Y(TABS_Y), text);
		AddPlace(left, top, right, bottom, W_TAB + f);
		x += (right - left) / W(1.0f) + 10.0f;
	}
	// (the number of skins listed, right of the tabs)
	char count[32];
	int32 n = 0;
	for (int32 r = 0; r < picker.numRows; r++)
		n += RowIsSkin(r);
	snprintf(count, sizeof(count), n > 1 ? Tr("%d skins", "%d apparences") : Tr("%d skin", "%d apparence"), n);
	GameText(count, text, ARRAY_SIZE(text));
	CFont::SetFontStyle(FONT_LOCALE(FONT_STANDARD));
	CFont::SetScale(W(0.34f), Y(0.7f));
	CFont::SetDropShadowPosition(1);
	CFont::SetRightJustifyOn();
	CFont::SetColor(Faded(GREY, alpha));
	CFont::PrintString(X(LIST_R - 4.0f), Y(TABS_Y + 6.0f), text);
	CFont::SetRightJustifyOff();
}

static void
DrawList(int32 alpha)
{
	const int32 lang = LangFrench() ? 1 : 0;
	CSprite2d::Draw2DPolygon(X(LIST_L), Y(LIST_T + 2.0f), X(LIST_R), Y(LIST_T - 4.0f), X(LIST_L - 4.0f), Y(LIST_B + 2.0f), X(LIST_R - 2.0f), Y(LIST_B + 6.0f),
		Faded(PANEL, alpha));
	const float bodyTop = LIST_T + 6.0f, bodyBottom = LIST_B - 4.0f;
	picker.visibleRows = (int32)((bodyBottom - bodyTop) / ROW_H);
	const int32 visible = picker.visibleRows;
	picker.first = Clamp(picker.first, 0, Max(0, picker.numRows - visible));
	const float left = X(LIST_L + 10.0f), right = X(LIST_R - 16.0f);
	wchar text[64];
	CFont::SetDropShadowPosition(1);
	CFont::SetDropColor(Faded(BLACK, alpha));
	for (int32 k = 0; k < visible && picker.first + k < picker.numRows; k++) {
		const int32 r = picker.first + k, skin = picker.rows[r];
		const float y = bodyTop + k * ROW_H, top = Y(y), bottom = Y(y + ROW_H);
		if (skin < 0) {
			// (a category's heading: the menus' blue, a line under it)
			GameText(catNames[-1 - skin][lang], text, ARRAY_SIZE(text));
			CFont::SetFontStyle(FONT_LOCALE(FONT_HEADING));
			CFont::SetScale(W(0.46f), Y(0.78f));
			CFont::SetColor(Faded(CYAN, alpha));
			CFont::PrintString(left - W(4.0f), Y(y + 1.0f), text);
			CSprite2d::DrawRect(CRect(left - W(4.0f), Y(y + ROW_H - 1.5f), right, Y(y + ROW_H - 1.5f) + Max(1.0f, Y(1.0f))), Faded(CRGBA(97, 194, 247, 110), alpha));
			continue;
		}
		const bool selected = skin == picker.selected, hovered = Hovered(left - W(6.0f), top, right + W(4.0f), bottom);
		if (selected)
			CSprite2d::DrawRect(CRect(left - W(6.0f), top + Y(0.5f), right + W(4.0f), bottom - Y(0.5f)), Faded(GREEN, alpha));
		else if (hovered)
			CSprite2d::DrawRect(CRect(left - W(6.0f), top + Y(0.5f), right + W(4.0f), bottom - Y(0.5f)), Faded(CRGBA(25, 130, 70, 90), alpha));
		AddPlace(left - W(6.0f), top, right + W(4.0f), bottom, r);
		Skins_Label(skins[skin].model, text, ARRAY_SIZE(text));
		CFont::SetFontStyle(FONT_LOCALE(FONT_STANDARD));
		CFont::SetScale(W(0.4f), Y(0.76f));
		CFont::SetColor(Faded(selected ? WHITE : PINK, alpha));
		CFont::PrintString(left + W(6.0f), Y(y + 1.5f), text);
		if (skin == picker.current) {
			GameText(Tr("current", "actuelle"), text, ARRAY_SIZE(text));
			CFont::SetScale(W(0.32f), Y(0.68f));
			CFont::SetRightJustifyOn();
			CFont::SetColor(Faded(selected ? WHITE : CYAN, alpha));
			CFont::PrintString(right - W(2.0f), Y(y + 2.5f), text);
			CFont::SetRightJustifyOff();
		}
	}
	// (the scrollbar, as the menus' lists have theirs)
	picker.barLeft = X(LIST_R - 12.0f);
	picker.barRight = X(LIST_R - 6.0f);
	picker.barTop = Y(bodyTop);
	picker.barBottom = Y(bodyBottom);
	if (picker.numRows > visible) {
		CSprite2d::DrawRect(CRect(picker.barLeft, picker.barTop, picker.barRight, picker.barBottom), Faded(CRGBA(0, 0, 0, 90), alpha));
		const float span = picker.barBottom - picker.barTop;
		const float top = picker.barTop + span * picker.first / picker.numRows, size = Max(Y(8.0f), span * visible / picker.numRows);
		CSprite2d::DrawRect(CRect(picker.barLeft, top, picker.barRight, Min(picker.barBottom, top + size)), Faded(PINK, alpha));
	}
}

static void
DrawPreview(int32 alpha, bool active)
{
	// (a darker panel behind the model, as the map's frame)
	CSprite2d::DrawRect(CRect(X(VIEW_L), Y(VIEW_T), X(VIEW_R), Y(VIEW_B)), Faded(CRGBA(0, 0, 0, 110), alpha),
		Faded(CRGBA(0, 0, 0, 110), alpha), Faded(CRGBA(49, 101, 148, 120), alpha), Faded(CRGBA(49, 101, 148, 120), alpha));
	const float centre = X((VIEW_L + VIEW_R) / 2.0f);
	wchar text[64];
	const int32 skin = picker.selected;
	Model *m = skin >= 0 ? CachedModel(skin) : nil;
	const int32 state = PreviewState();
	if (active && m && m->state == P_READY && alpha >= 255) {
		// (a slow turn: a full one in 18 s; or held at an angle, for the checks)
		const double now = CTimer::GetTimeInMillisecondsPauseMode();
		if (preview.lastTurn > 0.0)
			preview.angle = (float)fmod(preview.angle + (now - preview.lastTurn) * 0.02, 360.0);
		preview.lastTurn = now;
		RenderModel(*m, centre, Y(VIEW_B - 44.0f), Y(VIEW_B - VIEW_T - 58.0f), preview.held ? preview.heldAngle : preview.angle);
	} else if (state == P_LOADING || state == P_FAILED) {
		// (the placeholder: the model on its way, or none)
		char dots[8] = "";
		const int32 n = (int32)(CTimer::GetTimeInMillisecondsPauseMode() / 400) % 4;
		for (int32 i = 0; i < n; i++)
			dots[i] = '.';
		char line[48];
		snprintf(line, sizeof(line), state == P_FAILED ? "%s" : "%s%s", state == P_FAILED ? Tr("No preview", "Aperçu indisponible") : Tr("Loading", "Chargement"), dots);
		GameText(line, text, ARRAY_SIZE(text));
		CFont::SetFontStyle(FONT_LOCALE(FONT_STANDARD));
		CFont::SetScale(W(0.42f), Y(0.86f));
		CFont::SetDropShadowPosition(1);
		CFont::SetDropColor(Faded(BLACK, alpha));
		CFont::SetColor(Faded(GREY, alpha));
		CFont::SetCentreOn();
		CFont::SetCentreSize(W(VIEW_R - VIEW_L));
		CFont::PrintString(centre, Y((VIEW_T + VIEW_B) / 2.0f - 20.0f), text);
		CFont::SetCentreOff();
	}
	if (skin < 0)
		return;
	// (its name under it, its category and gender under that)
	Skins_Label(skins[skin].model, text, ARRAY_SIZE(text));
	CFont::SetFontStyle(FONT_LOCALE(FONT_HEADING));
	CFont::SetScale(W(0.6f), Y(1.0f));
	const float width = CFont::GetStringWidth(text, true), room = W(VIEW_R - VIEW_L - 12.0f);
	if (width > room)
		CFont::SetScale(W(0.6f) * room / width, Y(1.0f));
	CFont::SetCentreOn();
	CFont::SetCentreSize(W(VIEW_R - VIEW_L));
	CFont::SetDropShadowPosition(2);
	CFont::SetDropColor(Faded(BLACK, alpha));
	CFont::SetColor(Faded(PINK, alpha));
	CFont::PrintString(centre, Y(VIEW_B - 40.0f), text);
	char line[96];
	const int32 lang = LangFrench() ? 1 : 0;
	snprintf(line, sizeof(line), "%s  -  %s", catNames[skins[skin].cat][lang],
	         skins[skin].gender == G_FEMALE ? Tr("woman", "femme") : Tr("man", "homme"));
	GameText(line, text, ARRAY_SIZE(text));
	CFont::SetFontStyle(FONT_LOCALE(FONT_STANDARD));
	CFont::SetScale(W(0.34f), Y(0.7f));
	CFont::SetDropShadowPosition(1);
	CFont::SetColor(Faded(GREY, alpha));
	CFont::PrintString(centre, Y(VIEW_B - 18.0f), text);
	CFont::SetCentreOff();
}

static void
DrawButtons(int32 alpha)
{
	wchar text[160];
	CFont::SetFontStyle(FONT_LOCALE(FONT_HEADING));
	CFont::SetScale(W(BIGTEXT_X_SCALE), Y(BIGTEXT_Y_SCALE));
	CFont::SetDropShadowPosition(2);
	CFont::SetDropColor(Faded(BLACK, alpha));
	CFont::SetRightJustifyOn();
	// (Back at the right as the menus have it, Choose before it)
	float x = X(LIST_R - 2.0f);
	for (int32 b = 0; b < 2; b++) {
		const bool back = b == 0;
		text[0] = 0;
		if (back)
			Append(text, ARRAY_SIZE(text), TheText.Get("FEDS_TB"));
		else
			GameText(Tr("Choose", "Choisir"), text, ARRAY_SIZE(text));
		const float width = CFont::GetStringWidth(text, true), top = Y(BUTTONS_Y - 2.0f), bottom = Y(BUTTONS_Y + 20.0f);
		const bool hovered = Hovered(x - width - W(4.0f), top, x + W(4.0f), bottom);
		CFont::SetColor(Faded(hovered ? WHITE : PINK, alpha));
		CFont::PrintString(x, Y(BUTTONS_Y), text);
		AddPlace(x - width - W(4.0f), top, x + W(4.0f), bottom, back ? W_BACK : W_CHOOSE);
		x -= width + W(24.0f);
	}
	CFont::SetRightJustifyOff();
	// (the keys, small, at the left)
	GameText(Tr("Up/Down: select   Left/Right: men, women, all   Enter: choose   Esc: back",
	            "Haut/Bas : choisir   Gauche/Droite : hommes, femmes, tous   Entrée : valider   Echap : retour"), text, ARRAY_SIZE(text));
	CFont::SetFontStyle(FONT_LOCALE(FONT_STANDARD));
	CFont::SetScale(W(0.28f), Y(0.6f));
	CFont::SetDropShadowPosition(1);
	CFont::SetColor(Faded(GREY, alpha));
	CFont::PrintString(X(VIEW_L), Y(BUTTONS_Y + 24.0f), text);
}

void
Skins_PickerDraw(bool active)
{
	if (!picker.open)
		return;
	const int32 alpha = Clamp(FrontEndMenuManager.FadeIn(255), 0, 255);
	picker.numPlaces = 0;
	CFont::SetBackgroundOff();
	CFont::SetPropOn();
	CFont::SetJustifyOff();
	CFont::SetWrapx(SCREEN_WIDTH);
	CFont::SetRightJustifyWrap(0.0f);
	DrawTabs(alpha);
	DrawList(alpha);
	DrawPreview(alpha, active);
	DrawButtons(alpha);
}

// ---------------------------------------------------------------- tests
bool
Skins_TestCommand(const char *word, const char *arg)
{
	if (strcmp(word, "mpskinlist") == 0) {
		SortSkins();
		for (int32 k = 0; k < NUM_SKINS; k++) {
			const SkinDef &s = skins[picker.order[k]];
			debug("skins: %s %s %s '%s' / '%s'\n", catNames[s.cat][0], s.gender == G_FEMALE ? "F" : "M", s.model, s.en ? s.en : "", s.fr ? s.fr : "");
		}
		return true;
	}
	if (!picker.open) {
		if (strcmp(word, "mpfilter") == 0 || strcmp(word, "mppick") == 0 || strcmp(word, "mpconfirm") == 0 || strcmp(word, "mpcancel") == 0) {
			debug("%s: the picker is not open\n", word);
			return true;
		}
		return false;
	}
	if (strcmp(word, "mpfilter") == 0) {
		SetFilter(strcmp(arg, "male") == 0 ? F_MALE : strcmp(arg, "female") == 0 ? F_FEMALE : F_ALL);
		return true;
	}
	if (strcmp(word, "mppick") == 0) {
		const int32 i = Skins_Index(arg);
		if (i < 0) {
			debug("mppick: unknown skin '%s'\n", arg);
			return true;
		}
		if (!InFilter(i))
			SetFilter(F_ALL);
		Select(i);
		ShowRow(RowOf(i));
		return true;
	}
	if (strcmp(word, "mpconfirm") == 0) {
		Choose();
		return true;
	}
	if (strcmp(word, "mpturn") == 0) {
		// (the preview's turn held at an angle in degrees (0: facing), or "off")
		preview.held = strcmp(arg, "off") != 0;
		preview.heldAngle = (float)atof(arg);
		return true;
	}
	if (strcmp(word, "mpcancel") == 0) {
		Cancel();
		return true;
	}
	return false;
}

void
Skins_Debug(void)
{
	static const char *const filters[] = { "male", "female", "all" }, *const states[] = { "none", "loading", "ready", "failed" };
	int32 count = 0, cached = 0;
	for (int32 r = 0; r < picker.numRows; r++)
		count += RowIsSkin(r);
	for (int32 i = 0; i < CACHE; i++)
		cached += preview.models[i].state == P_READY;
	DebugAppend(",\"skins\":{\"open\":%s,\"filter\":\"%s\",\"rows\":%d,\"count\":%d,\"selected\":\"%s\",\"current\":\"%s\",\"first\":%d,"
	            "\"preview\":\"%s\",\"cached\":%d,\"anim\":\"%s\",\"total\":%d}",
	            picker.open ? "true" : "false", filters[picker.filter], picker.numRows, count,
	            picker.selected >= 0 ? skins[picker.selected].model : "", picker.current >= 0 ? skins[picker.current].model : "", picker.first,
	            states[PreviewState()], cached, states[preview.animState], NUM_SKINS);
}
}
#endif
