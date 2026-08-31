#pragma once

#include "nvse/GameTypes.h"

typedef UInt32 (*_TraitNameToID)(const char *traitName);
extern const _TraitNameToID TraitNameToID;

const char* __cdecl TraitIDToName(int id);	// slow

//	Tile			
//		TileRect		3C
//			TileMenu	40
//		TileImage		48
//		TileText		4C
//		Tile3D			50

class NiNode;
class Menu;

enum TileValues
{
	kTileValue_glow_branch = 0x6E,
	kTileValue_noglow_branch = 0x6F,
	kTileValue_Rect = 0x385,
	kTileValue_Image = 0x386,
	kTileValue_Text = 0x387,
	kTileValue_Nif = 0x388,
	kTileValue_Menu = 0x389,
	kTileValue_Hotrect = 0x38A,
	kTileValue_Window = 0x38B,
	kTileValue_Radial = 0x38C,
	kTileValue_Template = 0x3E7,
	kTileValue_Abs = 0x7DA,
	kTileValue_Add = 0x7D1,
	kTileValue_And = 0x7E2,
	kTileValue_Ceil = 0x7D9,
	kTileValue_Copy = 0x7D0,
	kTileValue_Div = 0x7D4,
	kTileValue_Eq = 0x7DE,
	kTileValue_Floor = 0x7D8,
	kTileValue_Gt = 0x7DC,
	kTileValue_Gte = 0x7DD,
	kTileValue_Lt = 0x7E0,
	kTileValue_Lte = 0x7E1,
	kTileValue_Max = 0x7D6,
	kTileValue_Min = 0x7D5,
	kTileValue_Mod = 0x7D7,
	kTileValue_Mul = 0x7D3,
	kTileValue_Neq = 0x7DF,
	kTileValue_Not = 0x7E4,
	kTileValue_OnlyIf = 0x7E5,
	kTileValue_OnlyIfNot = 0x7E6,
	kTileValue_Or = 0x7E3,
	kTileValue_Ref = 0x7E7,
	kTileValue_Round = 0x7DB,
	kTileValue_Sub = 0x7D2,
	kTileValue_Name = 0xBBA,
	kTileValue_Src = 0xBBB,
	kTileValue_Trait = 0xBBC,
	kTileValue_Value = 0xBB9,

	kTileValue_x =					0xFA1,
	kTileValue_y,
	kTileValue_visible,
	kTileValue_class,
	kTileValue_clipwindow =			0xFA6,
	kTileValue_stackingtype,
	kTileValue_locus,
	kTileValue_alpha,
	kTileValue_id,
	kTileValue_disablefade,
	kTileValue_listindex,
	kTileValue_depth,
	kTileValue_clips,
	kTileValue_target,
	kTileValue_height,
	kTileValue_width,
	kTileValue_red,
	kTileValue_green,
	kTileValue_blue,
	kTileValue_tile,
	kTileValue_childcount,
	kTileValue_child_count =		kTileValue_childcount,
	kTileValue_justify,
	kTileValue_zoom,
	kTileValue_font,
	kTileValue_wrapwidth,
	kTileValue_wraplimit,
	kTileValue_wraplines,
	kTileValue_pagenum,
	kTileValue_ishtml,
	kTileValue_cropoffsety,
	kTileValue_cropy =				kTileValue_cropoffsety,
	kTileValue_cropoffsetx,
	kTileValue_cropx =				kTileValue_cropoffsetx,
	kTileValue_menufade,
	kTileValue_explorefade,
	kTileValue_mouseover,
	kTileValue_string,
	kTileValue_shiftclicked,
	kTileValue_clicked =			0xFC7,
	kTileValue_clicksound =			0xFCB,
	kTileValue_filename,
	kTileValue_filewidth,
	kTileValue_fileheight,
	kTileValue_repeatvertical,
	kTileValue_repeathorizontal,
	kTileValue_animation =			0xFD2,
	kTileValue_linecount =			0xDD4,
	kTileValue_pagecount,
	kTileValue_xdefault,
	kTileValue_xup,
	kTileValue_xdown,
	kTileValue_xleft,
	kTileValue_xright,
	kTileValue_xbuttona =			0xFDD,
	kTileValue_xbuttonb,
	kTileValue_xbuttonx,
	kTileValue_xbuttony,
	kTileValue_xbuttonlt,
	kTileValue_xbuttonrt,
	kTileValue_xbuttonlb,
	kTileValue_xbuttonrb,
	kTileValue_xbuttonstart =		0xFE7,
	kTileValue_mouseoversound,
	kTileValue_draggable,
	kTileValue_dragstartx,
	kTileValue_dragstarty,
	kTileValue_dragoffsetx,
	kTileValue_dragoffsety,
	kTileValue_dragdeltax,
	kTileValue_dragdeltay,
	kTileValue_dragx,
	kTileValue_dragy,
	kTileValue_wheelable,
	kTileValue_wheelmoved,
	kTileValue_systemcolor,
	kTileValue_brightness,
	kTileValue_linegap =			0xFF7,
	kTileValue_resolutionconverter,
	kTileValue_texatlas,
	kTileValue_rotateangle,
	kTileValue_rotateaxisx,
	kTileValue_rotateaxisy,

	kTileValue_user0 =				0x1004,
	kTileValue_user1,
	kTileValue_user2,
	kTileValue_user3,
	kTileValue_user4,
	kTileValue_user5,
	kTileValue_user6,
	kTileValue_user7,
	kTileValue_user8,
	kTileValue_user9,
	kTileValue_user10,
	kTileValue_user11,
	kTileValue_user12,
	kTileValue_user13,
	kTileValue_user14,
	kTileValue_user15,
	kTileValue_user16,

	kTileValue_max
};

// 38+
class Tile
{
public:
	Tile();
	~Tile();

	enum eTileID
	{
		kTileID_rect = 0x0385,
		kTileID_image,
		kTileID_text,
		kTileID_3D,
		kTileID_nif = kTileID_3D,
		kTileID_menu,

		// Not a Tile descendend
		kTileID_hotrect,
		kTileID_window,
		// This one descend from TileImage
		kTileID_radial,

		kTileID_max
	};

	MEMBER_FN_PREFIX(Tile);
	DEFINE_MEMBER_FN(SetStringValue, void, 0x00A01350, UInt32 valueID, const char* str, bool bPropagate);
	DEFINE_MEMBER_FN(SetFloatValue, void, 0x00A012D0, UInt32 valueID, float num, bool bPropagate);

	virtual Tile		*Destroy(bool doFree);
	virtual void		Init(Tile *parent, const char *name, Tile *replacedChild = nullptr);
	virtual NiNode		*CalcNode(void);
	virtual UInt32		GetType(void);		// returns one of kTileValue_XXX
	virtual const char	*GetTypeStr(void);	// 4-byte id
	virtual bool		Unk_05(UInt32 arg0, UInt32 arg1);
	virtual UInt32		UpdateField(UInt32 valueID, float floatValue, const char *strValue);
	virtual void		Unk_07(void);
	virtual TileShaderProperty*	GetShaderProperty(void);
	virtual void		Unk_09(UInt32 arg0, UInt32 arg1, UInt32 arg2);

	struct Value;

	enum ActionType
	{
		kAction_copy = 0x7D0,
		kAction_add,
		kAction_sub,
		kAction_mul,
		kAction_div,
		kAction_min,
		kAction_max,
		kAction_mod,
		kAction_floor,
		kAction_ceil,
		kAction_abs,
		kAction_round,
		kAction_gt,
		kAction_gte,
		kAction_eq,
		kAction_neq,
		kAction_lt,
		kAction_lte,
		kAction_and,
		kAction_or,
		kAction_not,
		kAction_onlyif,
		kAction_onlyifnot,
		kAction_ref,
		kAction_begin,
		kAction_end,
	};

	enum ChangeFlag
	{
		kChange_PositionOrRotation = 0x1,
		kChange_Text = 0x2,
		kChange_Visibility = 0x4,
		kChange_ColorOrBrightness = 0x8,
		kChange_Dimensions = 0x10,
		kChange_ImageFilename = 0x20,
		kChange_NifFilename = 0x40,
		kChange_ClipWindow = 0x80,
		kChange_Clips = 0x100,
		kChange_Locus = 0x200,
		kChange_All = 0x3FF,
	};

	// 0C
	class Action
	{
	public:
		Action();
		~Action();

		virtual float	GetFloat();
		virtual Value	*GetValue();

		UInt32		type;		// 04
		Action		*next;		// 08
	};

	// 10
	class RefValueAction : public Action
	{
	public:
		RefValueAction();
		~RefValueAction();

		Value		*tileVal;	// 0C
	};

	// 10
	class FloatAction : public Action
	{
	public:
		FloatAction();
		~FloatAction();

		float		value;		// 0C
	};

	// 14
	struct Value
	{
		UInt32		id;			// 00
		Tile		*parent;	// 04
		float		num;		// 08
		char		*str;		// 0C
		Action		*action;	// 10

		void SetFloat(float fltVal, bool bRemoveFromReactionMap = true);
		void SetString(const char *strVal, bool bRemoveFromReactionMap = true);
		void Refresh(bool forceRefreshReactions = false) { ThisCall(0xA09410, this, forceRefreshReactions); };
		void AddRefValueAction(UInt32 _action, Tile* ref, UInt32 value) { ThisCall(0xA09130, this, _action, ref, value); };
		void AddFloatAction(UInt32 _action, float value) { ThisCall(0xA09080, this, _action, value); };
		void RemoveFromReactionMap() { ThisCall(0xA09200, this); };
	};

	struct Reaction
	{
		Value* react;
		Reaction* next;
	};

	DList<Tile>					children;		// 04
	BSSimpleArray<Value*>		values;			// 10
	String						name;			// 20
	Tile						*parent;		// 28
	NiNode						*node;			// 2C
	UInt32						flags;			// 30
	UInt8						unk34;			// 34
	UInt8						unk35;			// 35
	UInt8						pad35[2];		// 36

	static UInt32	TraitNameToID(const char *traitName);
	static UInt32	TraitNameToIDAdd(const char *traitName);
	Value* __fastcall GetValue(UInt32 typeID);
	Value			*GetValueName(const char *valueName);
	float			GetValueFloat(UInt32 id);
	const char* GetValueString(UInt32 id);
	DListNode<Tile>	*GetNthChild(UInt32 index);
	Tile			*GetChild(const char *childName);
	Tile			*GetComponent(const char *componentTile, const char *&trait);
	Tile			*GetComponentTile(const char *componentTile);
	Value			*GetComponentValue(const char *componentPath);
	Tile			*ReadXML(const char *xmlPath);
	Tile			*ReadXML_SkipUpdate(const char *xmlPath);
	char			*GetComponentFullName(char *resStr);
	void			SetFloat(UInt32 id, float fltVal, bool bRemoveFromReactionMap = true);
	void			SetVisible(bool isVisible, bool bRemoveFromReactionMap = true);
	void			SetString(UInt32 id, const char *strVal, bool bRemoveFromReactionMap = true);
	void			SetString(const char* strVal, bool bRemoveFromReactionMap = true);
	void			SetAlpha(float fAlpha);
	float			GetAlpha();
	Menu			*GetParentMenu();
	void			DestroyAllChildren();
	void			PokeValue(UInt32 valueID);
	void			FakeClick();

	void			Dump();
	void StartGradualSetTrait(TileValues trait, float startValue, float endValue, float duration, int type);
	bool IsInGradualSetTrait(TileValues trait);
	void EndGradualSetTrait(UInt32 trait = 0x80000000);
	Tile* GetByTraitName(const char* traitName) { return CdeclCall<Tile*>(0xA08B20, this, traitName); };
	Menu* GetMenu() { return ThisCall<Menu*>(0xA03C90, this); };
	Tile* GetChildByID(UInt32 id) { return ThisCall<Tile*>(0xA03EB0, this, id); };
	Tile* GetChildByName(const char* _name);
	Tile* GetParentByID(UInt32 id);
	void SetChangeFlag(ChangeFlag flag = kChange_All) { ThisCall(0xA07530, this, flag); };
	void ApplyAlphaTexture(const char* texturePath);
	float GetLocusAdjustedPosX();
	float GetLocusAdjustedPosY();
	void PlayClickSound();
	bool IsVisible();
	void SetParent(Tile* newParent, Tile* oldParent);

	static Tile* __cdecl CreateByID(eTileID eType);
};

// 3C
class TileRect : public Tile
{
public:
	UInt32	unk38;	// 38
};

// 40
class TileMenu : public TileRect
{
public:
	Menu	*menu;	// 3C
};

class TileShaderProperty;

// 48
class TileImage : public Tile
{
public:
	float				flt038;			// 38
	NiSourceTexture*	texture;		// 3C
	TileShaderProperty	*shaderProp;	// 40
	UInt8				byt044;			// 44
	UInt8				fill[3];		// 45-47
	void SetTexture(NiSourceTexture* tex);
	static TileImage* Create();
};

class TileText : public Tile
{
public:
};

class Tile3D : public Tile
{
public:
	NiControllerManager* controllerManager;
	BSAnimGroupSequence* animSequence;
	String animationStr;
	String meshPath;
};

void Debug_DumpTraits(void);