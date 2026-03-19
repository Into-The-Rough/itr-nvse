//quick view notes and play holotapes on pickup without opening pip-boy

#include "nvse/PluginAPI.h"
#include "nvse/GameUI.h"
#include "QuickReadNote.h"
#include "internal/SafeWrite.h"
#include "internal/EngineFunctions.h"

#include "internal/globals.h"
#include "internal/CallTemplates.h"
#include "handlers/DialogueTextFilter.h"

static void QRN_SafeWrite32(UInt32 addr, UInt32 data) {
	DWORD oldProtect;
	VirtualProtect(reinterpret_cast<void*>(addr), 4, PAGE_EXECUTE_READWRITE, &oldProtect);
	*reinterpret_cast<UInt32*>(addr) = data;
	VirtualProtect(reinterpret_cast<void*>(addr), 4, oldProtect, &oldProtect);
}

namespace QuickReadNote
{
	static int g_timeoutMs = 5000;
	static int g_controlID = 6;
	static int g_maxLines = 0;

	static BGSNote* g_pendingNote = nullptr;
	static UInt8 g_pendingNoteType = 0;
	static DWORD g_noteAddedTime = 0;
	static bool g_controlWasPressed = false;
	static char g_truncatedBuffer[4096];
	static bool g_noteWasTruncated = false;
	static BGSNote* g_truncatedNote = nullptr;
	static bool g_openPipBoyPending = false;
	static bool g_switchToMiscPending = false;
	static bool g_noHolotapeStopSound = false;

	constexpr UInt32 kVtbl_MessageMenu = 0x107566C;
	constexpr UInt32 kOffset_HandleClick = 0x0C;

	#define GameHeapAlloc(size) ((void*(__thiscall*)(void*, UInt32))(0xAA3E40))((void*)0x11F6238, size)
	#define GameHeapFree(ptr) ((void(__thiscall*)(void*, void*))(0xAA4060))((void*)0x11F6238, ptr)
	#define InterfaceManager_Singleton (*(void**)0x11D8A80)
	#define g_mapMenuPtr (*(void**)0x11DA368)
	#define GAME_SCREEN_HEIGHT (*(UInt32*)0x11F9434)

	struct BSSoundHandle {
		UInt32 uiSoundID;  //0x00
		bool bAssumeSuccess; //0x04
		UInt32 uiState;    //0x08
		BSSoundHandle() : uiSoundID(0xFFFFFFFF), bAssumeSuccess(false), uiState(0) {}
		bool IsValid() const { return uiSoundID != 0xFFFFFFFF; }
		bool IsPlaying() const {
			if (!IsValid()) return false;
			return Engine::BSSoundHandle_IsPlaying(const_cast<BSSoundHandle*>(this));
		}
		bool Play(bool abUnk) {
			if (!IsValid()) return false;
			return Engine::BSSoundHandle_Play(this, abUnk);
		}
		void Stop() {
			if (!IsValid()) return;
			Engine::BSSoundHandle_Stop(this);
		}
		void SetVolume(float volume) {
			if (!IsValid()) return;
			Engine::BSSoundHandle_SetVolume(this, volume);
		}
	};
	static_assert(offsetof(BSSoundHandle, uiSoundID) == 0x00);
	static_assert(offsetof(BSSoundHandle, bAssumeSuccess) == 0x04);
	static_assert(offsetof(BSSoundHandle, uiState) == 0x08);

	struct SoundList {
		BSSoundHandle data;
		SoundList* next;
		void Append(BSSoundHandle* sound) { ThisCall<void>(0x7A19A0, this, sound); }
		void FreeAll() { ThisCall<void>(0x76B7A0, this); }
	};

	struct BSSimpleArrayChar {
		void* vtable;
		char* data;
		UInt32 size;
		UInt32 capacity;
	};

	struct BSWin32Audio {
		static BSWin32Audio* GetSingleton() { return *(BSWin32Audio**)0x11F6D98; }
		enum AudioFlags : UInt32 {
			kAudioFlags_2D = 0x1,
			kAudioFlags_100 = 0x100,
			kAudioFlags_SystemSound = 0x800
		};
		BSSoundHandle GetSoundHandleByFormID(UInt32 formID, UInt32 flags) {
			BSSoundHandle handle;
			ThisCall<void>(0xAD73B0, this, &handle, formID, flags);
			return handle;
		}
		BSSoundHandle GetSoundHandleByFilePath(const char* filePath, UInt32 flags, void* sound) {
			BSSoundHandle handle;
			ThisCall<void>(0xAD7480, this, &handle, filePath, flags, sound);
			return handle;
		}
		BSSoundHandle GetSoundHandleByEditorName(const char* editorName, UInt32 flags) {
			BSSoundHandle handle;
			ThisCall<void>(0xAD7550, this, &handle, editorName, flags);
			return handle;
		}
		void FadeInDialogueSound() { ThisCall<void>(0xAD85A0, this); }
		void FadeOutDialogueSound() { ThisCall<void>(0xAD8650, this); }
	};

	struct BSString {
		char* str;
		UInt16 len;
		UInt16 bufLen;
		const char* c_str() const { return str ? str : ""; }
	};

	struct DialogueResponse {
		BSString strResponseText;   //0x00
		UInt32 uiEmotionType;       //0x08
		UInt32 uiEmotionValue;      //0x0C
		BSString strVoiceFilePath;  //0x10
		void* pSpeakerAnimation;
		void* pListenerAnimation;
		void* pSound;
		UInt8 ucFlags;
		UInt8 pad25[3];
		UInt32 uiResponseNumber;
	};
	static_assert(offsetof(DialogueResponse, strResponseText) == 0x00);
	static_assert(offsetof(DialogueResponse, strVoiceFilePath) == 0x10);

	template <class T> struct BSSimpleList {
		T item;
		BSSimpleList<T>* next;
	};

	struct DialogueItem {
		BSSimpleList<DialogueResponse*> kResponses;
		BSSimpleList<DialogueResponse*>* pCurrentResponse;
		void* pTopicInfo;
		void* pTopic;
		void* pQuest;
		void* pSpeaker;
		bool FirstResponse() {
			pCurrentResponse = &kResponses;
			return kResponses.item != nullptr;
		}
		bool NextResponse() {
			if (pCurrentResponse && pCurrentResponse->next) {
				pCurrentResponse = pCurrentResponse->next;
				return pCurrentResponse->item != nullptr;
			}
			return false;
		}
		DialogueResponse* GetCurrentItem() const {
			return pCurrentResponse ? pCurrentResponse->item : nullptr;
		}
	};

	struct Conversation {
		BSSimpleList<DialogueItem*> kDialogueItems;
		BSSimpleList<DialogueItem*>* pCurrentItem;
		bool FirstItem() {
			pCurrentItem = &kDialogueItems;
			return kDialogueItems.item != nullptr;
		}
		DialogueItem* GetCurrentItem() const {
			return pCurrentItem ? pCurrentItem->item : nullptr;
		}
	};

	struct Character {
		UInt8 pad00[0x08];
		UInt32 flags;
		UInt8 pad0C[0x1BC];
	};

	constexpr UInt32 kMapMenu_currentNote = 0x90;
	constexpr UInt32 kMapMenu_holotapeDialogues = 0x98;
	constexpr UInt32 kMapMenu_holotapeSubtitles = 0xA8;
	constexpr UInt32 kMapMenu_isHolotapeVoicePlaying = 0xBC;
	constexpr UInt32 kMapMenu_noteList_head = 0x164;

	struct ListBoxItem {
		Tile* tile;
		void* object;
		UInt8 isFiltered;
		UInt8 pad[3];
	};

	struct ListNode {
		ListBoxItem* data;
		ListNode* next;
	};

	static void ClearHUDSubtitles() {
		void** g_hudMainMenu = (void**)0x11D96C0;
		if (*g_hudMainMenu)
			ThisCall<void>(0x775670, *g_hudMainMenu);
	}

	static void StopHolotape(void* map) {
		SoundList** currentSound = (SoundList**)((UInt8*)map + 0xB8); //currentHolotapeDialogueSound
		if (*currentSound && (*currentSound)->data.IsPlaying())
			(*currentSound)->data.Stop();
		SoundList* dialogues = (SoundList*)((UInt8*)map + kMapMenu_holotapeDialogues);
		dialogues->FreeAll();
		BSSimpleArrayChar* subtitles = (BSSimpleArrayChar*)((UInt8*)map + kMapMenu_holotapeSubtitles);
		ThisCall<void>(0x7A1C30, subtitles, 1);
		*currentSound = nullptr;
		*(float*)((UInt8*)map + 0xC0) = 0.0f; //holotapeTotalTime
		*(UInt32*)((UInt8*)map + 0xC4) = 0; //holotapePlayStartTime
		*(UInt8*)((UInt8*)map + kMapMenu_isHolotapeVoicePlaying) = 0;
		if (!g_noHolotapeStopSound) {
			BSSoundHandle handle = BSWin32Audio::GetSingleton()->GetSoundHandleByEditorName(
				"UIPipBoyHolotapeStop",
				BSWin32Audio::kAudioFlags_100 | BSWin32Audio::kAudioFlags_SystemSound | BSWin32Audio::kAudioFlags_2D);
			handle.Play(false);
		}
		g_noHolotapeStopSound = false;
		BSWin32Audio::GetSingleton()->FadeOutDialogueSound();
		*(UInt8*)0x11DCFA4 = false;
		ClearHUDSubtitles();
	}

	static void PlayHolotape(BGSNote* note, bool playStartStopSound) {
		void* map = g_mapMenuPtr;
		if (!map) return;

		UInt8* isPlaying = (UInt8*)((UInt8*)map + kMapMenu_isHolotapeVoicePlaying);
		SoundList* dialogues = (SoundList*)((UInt8*)map + kMapMenu_holotapeDialogues);
		BSSimpleArrayChar* subtitles = (BSSimpleArrayChar*)((UInt8*)map + kMapMenu_holotapeSubtitles);

		if (*isPlaying)
			StopHolotape(map);

		UInt8 noteType = *(UInt8*)((UInt8*)note + 0x7C);
		if (noteType == 0) { //kSound
			void* voice = *(void**)((UInt8*)note + 0x6C);
			UInt32 voiceRefID = voice ? *(UInt32*)((UInt8*)voice + 0xC) : 0;
			if (voiceRefID) {
				BSSoundHandle sound = BSWin32Audio::GetSingleton()->GetSoundHandleByFormID(
					voiceRefID, BSWin32Audio::kAudioFlags_2D | BSWin32Audio::kAudioFlags_100);
				dialogues->Append(&sound);
				*isPlaying = true;
			}
		} else if (noteType == 3) { //kVoice
			Character* character = (Character*)GameHeapAlloc(sizeof(Character));
			ThisCall<void>(0x8D1F40, character, false);
			character->flags |= 0x00004000;
			void* speaker = *(void**)((UInt8*)note + 0x70);
			ThisCall<void>(0x575690, character, speaker);

			void* voice = *(void**)((UInt8*)note + 0x6C);
			Conversation* pConversation = (Conversation*)GameHeapAlloc(sizeof(Conversation));
			void** g_thePlayer = (void**)0x11DEA3C;
			ThisCall<void>(0x83B850, pConversation, character, *g_thePlayer, voice);

			UInt32 audioFlags = *(UInt32*)0x7974CA;
			pConversation->FirstItem();
			if (DialogueItem* currentItem = pConversation->GetCurrentItem()) {
				if (currentItem->FirstResponse()) {
					*isPlaying = true;
					do {
						currentItem = pConversation->GetCurrentItem();
						DialogueResponse* currentResponse = currentItem->GetCurrentItem();
						if (!currentResponse) break;
						BSString* voiceLineStr = &currentResponse->strResponseText;
						ThisCall<void>(0x7A1AC0, subtitles, voiceLineStr);
						void* topicInfo = currentItem->pTopicInfo;
						DTF_Suppress(true);
						ThisCall<void>(0x61F170, topicInfo, 0, character);
						DTF_Suppress(false);
						BSSoundHandle toPlay = BSWin32Audio::GetSingleton()->GetSoundHandleByFilePath(
							currentResponse->strVoiceFilePath.c_str(), audioFlags, nullptr);
						toPlay.SetVolume(0.9f);
						dialogues->Append(&toPlay);
						ThisCall<void>(0x61F170, topicInfo, 1, character);
					} while (currentItem->NextResponse());
				}
			}
			GameHeapFree(pConversation);
			GameHeapFree(character);
		}

		if (*isPlaying) {
			if (playStartStopSound) {
				BSSoundHandle sound = BSWin32Audio::GetSingleton()->GetSoundHandleByEditorName(
					"UIPipBoyHolotapeStart",
					BSWin32Audio::kAudioFlags_100 | BSWin32Audio::kAudioFlags_SystemSound | BSWin32Audio::kAudioFlags_2D);
				sound.Play(false);
			} else {
				g_noHolotapeStopSound = true;
			}
			*(UInt8*)0x11DCFA4 = true;
			BSWin32Audio::GetSingleton()->FadeInDialogueSound();
		}
	}

	static UInt32 GetMaxLines() {
		if (g_maxLines > 0) {
			if (g_maxLines < 8) return 8;
			if (g_maxLines > 48) return 48;
			return g_maxLines;
		}
		UInt32 screenHeight = GAME_SCREEN_HEIGHT;
		if (screenHeight < 480 || screenHeight > 4320)
			screenHeight = GetSystemMetrics(SM_CYSCREEN);
		if (screenHeight < 480 || screenHeight > 4320)
			return 16;
		UInt32 maxLines = (screenHeight * 16) / 1440;
		if (maxLines < 8) maxLines = 8;
		if (maxLines > 48) maxLines = 48;
		return maxLines;
	}

	static const char* TruncateNoteText(const char* text, BGSNote* note) {
		size_t len = strlen(text);
		UInt32 maxLines = GetMaxLines();
		const UInt32 CHARS_PER_LINE = 65;
		UInt32 lines = 1;
		UInt32 charsOnCurrentLine = 0;
		size_t breakPoint = 0;

		for (size_t i = 0; i < len; i++) {
			if (text[i] == '\n') {
				lines++;
				charsOnCurrentLine = 0;
				if (lines > maxLines) { breakPoint = i; break; }
			} else {
				charsOnCurrentLine++;
				if (charsOnCurrentLine >= CHARS_PER_LINE) {
					lines++;
					charsOnCurrentLine = 0;
					if (lines > maxLines) { breakPoint = i; break; }
				}
			}
		}

		if (breakPoint == 0) {
			g_noteWasTruncated = false;
			g_truncatedNote = nullptr;
			return text;
		}

		size_t wordBreak = breakPoint;
		while (wordBreak > 0 && text[wordBreak] != ' ' && text[wordBreak] != '\n')
			wordBreak--;
		if (wordBreak > breakPoint / 2)
			breakPoint = wordBreak;

		//clamp to leave room for suffix
		if (breakPoint > sizeof(g_truncatedBuffer) - 32)
			breakPoint = sizeof(g_truncatedBuffer) - 32;

		memcpy(g_truncatedBuffer, text, breakPoint);
		strcpy_s(g_truncatedBuffer + breakPoint, sizeof(g_truncatedBuffer) - breakPoint, "\n\n...[Note truncated]");
		g_noteWasTruncated = true;
		g_truncatedNote = note;
		return g_truncatedBuffer;
	}

	typedef bool(__cdecl* _ShowMessageBox)(const char*, UInt32, UInt32, void*, UInt32, UInt32, float, float, ...);
	static const _ShowMessageBox ShowMessageBox = (_ShowMessageBox)0x703E80;

	static BGSNote* g_noteToSelect = nullptr;

	static void SelectNoteInList(void* mapMenu, BGSNote* note) {
		if (!mapMenu || !note) return;

		UInt32 selectedTrait = Engine::Tile_TextToTrait("_selected");

		//noteList at 0x160 is ListBox which has vtable at +0, list at +4, selected at +10
		ListBoxItem* firstData = *(ListBoxItem**)((UInt8*)mapMenu + kMapMenu_noteList_head);
		ListNode* nextNode = *(ListNode**)((UInt8*)mapMenu + kMapMenu_noteList_head + 4);
		Tile** selectedPtr = (Tile**)((UInt8*)mapMenu + 0x170); //noteList selected
		BGSNote** currentNotePtr = (BGSNote**)((UInt8*)mapMenu + kMapMenu_currentNote);
		Tile* dataPanelTile = *(Tile**)((UInt8*)mapMenu + 0x5C); //tiles[13] data panel

		Tile* foundTile = nullptr;

		//check first inline node
		if (firstData && firstData->object == note && firstData->tile) {
			foundTile = firstData->tile;
		} else {
			//iterate remaining nodes
			ListNode* node = nextNode;
			while (node) {
				ListBoxItem* item = node->data;
				if (item && item->object == note && item->tile) {
					foundTile = item->tile;
					break;
				}
				node = node->next;
			}
		}

		if (!foundTile) return;

		if (*selectedPtr)
			Engine::Tile_SetFloat(*selectedPtr, selectedTrait, 0.0f, true);

		*selectedPtr = foundTile;
		*currentNotePtr = note;
		Engine::Tile_SetFloat(foundTile, selectedTrait, 1.0f, true);

		if (dataPanelTile)
			Engine::Tile_SetFloat(dataPanelTile, 0xFA5, 1.0f, true);

		//display note content
		ThisCall<void>(0x7993D0, mapMenu, note);
	}

	static void SwitchToMiscTab() {
		void* mapMenu = g_mapMenuPtr;
		if (!mapMenu) return;
		void* tiles17 = *(void**)((UInt8*)mapMenu + 0x6C); //tiles[17]
		if (!tiles17) return;
		UInt32 traitID = *(UInt32*)0x11DA360;
		if (traitID == 0 || traitID == 0xFFFFFFFF)
			traitID = Engine::Tile_TextToTrait("_CurrentTab");
		ThisCall<void>(0x700320, tiles17, traitID, 3);
		*(UInt8*)((UInt8*)mapMenu + 0x80) = 0x23; //currentTab = misc
		((void(__cdecl*)())0x79ABA0)();

		if (g_noteToSelect) {
			SelectNoteInList(mapMenu, g_noteToSelect);
			g_noteToSelect = nullptr;
		}
	}

	static void OpenPipBoyToNotes() {
		void* im = InterfaceManager_Singleton;
		if (im) {
			ThisCall<void>(0x70F4E0, im, nullptr, 0x3FF);
			g_switchToMiscPending = true;
		}
	}

	static void ShowNoteMenu(BGSNote* note) {
		if (!note) return;
		UInt8 noteType = *(UInt8*)((UInt8*)note + 0x7C);
		if (noteType == 1) { //kText
			void* noteText = *(void**)((UInt8*)note + 0x6C);
			if (noteText) {
				void** vtable = *(void***)noteText;
				typedef const char*(__thiscall* GetFn)(void*, void*, UInt32);
				GetFn getFn = (GetFn)vtable[4];
				const char* text = getFn(noteText, note, 'MANT');
				if (text && *text) {
					const char* displayText = TruncateNoteText(text, note);
					if (g_noteWasTruncated)
						ShowMessageBox(displayText, 0, 0, nullptr, 0, 0x17, 0, 0, "OK", "View Full", NULL);
					else
						ShowMessageBox(displayText, 0, 0, nullptr, 0, 0x17, 0, 0, "OK", NULL);
					*(UInt8*)((UInt8*)note + 0x7D) = 1;
				}
			}
		} else if (noteType == 0 || noteType == 3) { //kSound or kVoice
			void* map = g_mapMenuPtr;
			if (map) {
				PlayHolotape(note, true);
				*(BGSNote**)((UInt8*)map + kMapMenu_currentNote) = note;
				*(UInt8*)((UInt8*)note + 0x7D) = 1;
			}
		} else if (noteType == 2) { //kImage
			*(UInt8*)((UInt8*)note + 0x7D) = 1;
		}
	}

	typedef void(__thiscall* _MessageMenu_HandleClick)(void* menu, SInt32 tileID, Tile* clickedTile);
	static _MessageMenu_HandleClick ChainedHandleClick = nullptr;

	void __fastcall MessageMenu_HandleClick_Hook(void* menu, void* edx, SInt32 tileID, Tile* clickedTile) {
		bool wasOurMessage = g_noteWasTruncated && g_truncatedNote;
		BGSNote* noteToOpen = wasOurMessage ? g_truncatedNote : nullptr;
		if (wasOurMessage) {
			g_noteWasTruncated = false;
			g_truncatedNote = nullptr;
		}
		if (ChainedHandleClick)
			ChainedHandleClick(menu, tileID, clickedTile);
		if (wasOurMessage) {
			void* im = InterfaceManager_Singleton;
			if (im) {
				UInt8 buttonIndex = *(UInt8*)((UInt8*)im + 0xE4); //msgBoxButton
				if (buttonIndex == 1) {
					g_openPipBoyPending = true;
					g_noteToSelect = noteToOpen;
				}
			}
		}
	}

	void __cdecl OnNoteAddedCallback(BGSNote* note) {
		if (note) {
			g_pendingNote = note;
			g_pendingNoteType = *(UInt8*)((UInt8*)note + 0x7C);
			g_noteAddedTime = GetTickCount();
			g_controlWasPressed = false;
		}
	}

	static UInt32 s_originalNoteAddedCall = 0;

	const char* __fastcall ProcessNoteAdded(void* setting, BGSNote* note) {
		OnNoteAddedCallback(note);
		return ThisCall<const char*>(s_originalNoteAddedCall, setting);
	}

	__declspec(naked) void OnNoteAddedHook() {
		__asm {
			mov edx, [ebp+8]
			jmp ProcessNoteAdded
		}
	}

	static UInt32 s_originalQueueUIMessage = 0;
	static char s_modifiedMessage[512];

	static const char* kControlNames[] = {
		"Forward", "Backward", "Left", "Right", "Attack",
		"Activate", "Aim", "ReadyWeapon", "Crouch", "Run",
		"AlwaysRun", "AutoMove", "Jump", "TogglePOV", "MenuMode",
		"Rest", "VATS", "Hotkey1", "Hotkey2", "Hotkey3",
		"Hotkey4", "Hotkey5", "Hotkey6", "Hotkey7", "Hotkey8",
		"QuickSave", "QuickLoad", "Grab"
	};

	char __cdecl OnQueueUIMessageHook(char* msg, UInt32 emotion, char* imagePath,
		char* soundName, float time, char instantEnd) {
		if (msg && g_pendingNote) {
			const char* controlName = (g_controlID <= 27) ? kControlNames[g_controlID] : "Key";
			const char* action = (g_pendingNoteType == 0 || g_pendingNoteType == 3) ? "listen" : "view";
			snprintf(s_modifiedMessage, sizeof(s_modifiedMessage), "%s. Press %s to %s.", msg, controlName, action);
			msg = s_modifiedMessage;
		}
		typedef char(__cdecl* QueueUIMessageFn)(char*, UInt32, char*, char*, float, char);
		return ((QueueUIMessageFn)s_originalQueueUIMessage)(msg, emotion, imagePath, soundName, time, instantEnd);
	}

	class OSInputGlobals {
	public:
		bool GetControlState(UInt32 controlCode, UInt8 state) {
			return Engine::OSInputGlobals_GetControlState(this, controlCode, state);
		}
		static OSInputGlobals* GetSingleton() { return *(OSInputGlobals**)0x11F35CC; }
	};

	void Init(int timeoutMs, int controlID, int maxLines) {
		g_timeoutMs = timeoutMs;
		g_controlID = controlID;
		g_maxLines = maxLines;

		s_originalNoteAddedCall = SafeWrite::GetRelJumpTarget(0x966B0A);
		SafeWrite::WriteRelCall(0x966B0A, (UInt32)OnNoteAddedHook);

		s_originalQueueUIMessage = SafeWrite::GetRelJumpTarget(0x966B53);
		SafeWrite::WriteRelCall(0x966B53, (UInt32)OnQueueUIMessageHook);

		UInt32* vtbl = reinterpret_cast<UInt32*>(kVtbl_MessageMenu);
		ChainedHandleClick = reinterpret_cast<_MessageMenu_HandleClick>(vtbl[kOffset_HandleClick / 4]);
		QRN_SafeWrite32(kVtbl_MessageMenu + kOffset_HandleClick, reinterpret_cast<UInt32>(MessageMenu_HandleClick_Hook));

		Log("QuickReadNote installed (timeout=%dms, control=%d, maxLines=%d)",
			timeoutMs, controlID, maxLines);
	}

	void Update() {
		if (g_openPipBoyPending) {
			g_openPipBoyPending = false;
			OpenPipBoyToNotes();
		}
		if (g_switchToMiscPending && g_mapMenuPtr) {
			g_switchToMiscPending = false;
			SwitchToMiscTab();
		}
		if (!g_pendingNote) return;

		DWORD currentTime = GetTickCount();
		DWORD elapsed = currentTime - g_noteAddedTime;
		if (elapsed >= (DWORD)g_timeoutMs) {
			g_pendingNote = nullptr;
			return;
		}

		OSInputGlobals* input = OSInputGlobals::GetSingleton();
		if (!input) return;
		bool isPressed = input->GetControlState(g_controlID, 0);
		if (isPressed && !g_controlWasPressed) {
			//typeID 0x31 = BGSNote
			if (*(UInt8*)((UInt8*)g_pendingNote + 4) == 0x31)
				ShowNoteMenu(g_pendingNote);
			g_pendingNote = nullptr;
		}
		g_controlWasPressed = isPressed;
	}

	void UpdateSettings(int timeoutMs, int controlID, int maxLines) {
		g_timeoutMs = timeoutMs;
		g_controlID = controlID;
		g_maxLines = maxLines;
	}
}

void QuickReadNote_Init(int timeoutMs, int controlID, int maxLines) {
	QuickReadNote::Init(timeoutMs, controlID, maxLines);
}

void QuickReadNote_Update() {
	QuickReadNote::Update();
}

void QuickReadNote_UpdateSettings(int timeoutMs, int controlID, int maxLines) {
	QuickReadNote::UpdateSettings(timeoutMs, controlID, maxLines);
}
