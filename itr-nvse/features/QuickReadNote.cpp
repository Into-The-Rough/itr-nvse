//quick view notes and play holotapes on pickup without opening pip-boy

#include <Windows.h>
#include "common/ITypes.h"
#include <cstdio>
#include <cstring>
#include "QuickReadNote.h"
#include "internal/SafeWrite.h"
#include "internal/Detours.h"
#include "internal/EngineFunctions.h"
#include "internal/GameGlobals.h"
#include "internal/DialogueLayout.h"
#include "internal/MenuLayout.h"

#include "internal/globals.h"
#include "internal/CallTemplates.h"
#include "MessageBoxQuickClose.h"
#include "handlers/DialogueTextFilter.h"

class BGSNote;
class Tile;

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

	struct BSSoundHandle {
		UInt32 uiSoundID; //0x00
		bool bAssumeSuccess; //0x04
		UInt32 uiState; //0x08
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
		static BSWin32Audio* GetSingleton() { return static_cast<BSWin32Audio*>(GetBSWin32Audio()); }
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

	static bool ConversationFirstItem(ConversationView* conversation) {
		return ThisCall<bool>(0x83B9A0, conversation);
	}

	static DialogueItemView* ConversationGetCurrentItem(ConversationView* conversation) {
		return ThisCall<DialogueItemView*>(0x83C820, conversation);
	}

	static bool DialogueItemFirstResponse(DialogueItemView* item) {
		return ThisCall<bool>(0x83C7B0, item);
	}

	static bool DialogueItemNextResponse(DialogueItemView* item) {
		return ThisCall<bool>(0x83C7E0, item);
	}

	static DialogueResponseView* DialogueItemGetCurrentItem(DialogueItemView* item) {
		return ThisCall<DialogueResponseView*>(0x83C820, item);
	}

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
		if (void* hud = GetHUDMainMenu())
			ThisCall<void>(0x775670, hud);
	}

	static void StopHolotape(void* map) {
		auto* mapView = MapMenuAsView(map);
		auto* currentSound = static_cast<SoundList*>(mapView->currentHolotapeDialogueSound);
		if (currentSound && currentSound->data.IsPlaying())
			currentSound->data.Stop();
		auto* dialogues = reinterpret_cast<SoundList*>(mapView->holotapeDialogues);
		dialogues->FreeAll();
		auto* subtitles = reinterpret_cast<BSSimpleArrayChar*>(mapView->holotapeSubtitles);
		ThisCall<void>(0x7A1C30, subtitles, 1);
		mapView->currentHolotapeDialogueSound = nullptr;
		mapView->holotapeTotalTime = 0.0f;
		mapView->holotapePlayStartTime = 0;
		mapView->isHolotapeVoicePlaying = 0;
		if (!g_noHolotapeStopSound) {
			BSSoundHandle handle = BSWin32Audio::GetSingleton()->GetSoundHandleByEditorName(
				"UIPipBoyHolotapeStop",
				BSWin32Audio::kAudioFlags_100 | BSWin32Audio::kAudioFlags_SystemSound | BSWin32Audio::kAudioFlags_2D);
			handle.Play(false);
		}
		g_noHolotapeStopSound = false;
		BSWin32Audio::GetSingleton()->FadeOutDialogueSound();
		SetInDialogueOrHolotapePlaying(false);
		ClearHUDSubtitles();
	}

	static void PlayHolotape(BGSNote* note, bool playStartStopSound) {
		void* map = GetMapMenu();
		if (!map) return;
		auto* mapView = MapMenuAsView(map);

		UInt8* isPlaying = &mapView->isHolotapeVoicePlaying;
		auto* dialogues = reinterpret_cast<SoundList*>(mapView->holotapeDialogues);
		auto* subtitles = reinterpret_cast<BSSimpleArrayChar*>(mapView->holotapeSubtitles);

		if (*isPlaying)
			StopHolotape(map);

		UInt8 noteType = BGSNoteGetType(note);
		if (noteType == kBGSNoteType_Sound) {
			UInt32 voiceRefID = BGSNoteGetVoiceRefID(note);
			if (voiceRefID) {
				BSSoundHandle sound = BSWin32Audio::GetSingleton()->GetSoundHandleByFormID(
					voiceRefID, BSWin32Audio::kAudioFlags_2D | BSWin32Audio::kAudioFlags_100);
				dialogues->Append(&sound);
				*isPlaying = true;
			}
		} else if (noteType == kBGSNoteType_Voice) {
			CharacterView* character = static_cast<CharacterView*>(Engine::GameHeapAlloc(sizeof(CharacterView)));
			ThisCall<void>(0x8D1F40, character, false);
			character->flags |= 0x00004000;
			ThisCall<void>(0x575690, character, BGSNoteGetSpeaker(note));

			void* voice = BGSNoteGetVoice(note);
			ConversationView* pConversation = static_cast<ConversationView*>(Engine::GameHeapAlloc(sizeof(ConversationView)));
			ThisCall<void>(0x83B850, pConversation, character, *(void**)g_thePlayerPtr, voice);

			UInt32 audioFlags = GetMapMenuVoicePlaybackAudioFlags();
			ConversationFirstItem(pConversation);
			if (DialogueItemView* currentItem = ConversationGetCurrentItem(pConversation)) {
				if (DialogueItemFirstResponse(currentItem)) {
					*isPlaying = true;
					do {
						currentItem = ConversationGetCurrentItem(pConversation);
						DialogueResponseView* currentResponse = DialogueItemGetCurrentItem(currentItem);
						if (!currentResponse) break;
						DialogueStringView* voiceLineStr = &currentResponse->responseText;
						ThisCall<void>(0x7A1AC0, subtitles, voiceLineStr);
						void* topicInfo = currentItem->currentTopicInfo;
						DialogueTextFilter::Suppress(true);
						ThisCall<void>(0x61F170, topicInfo, 0, character);
						DialogueTextFilter::Suppress(false);
						BSSoundHandle toPlay = BSWin32Audio::GetSingleton()->GetSoundHandleByFilePath(
							currentResponse->voiceFileName.c_str(), audioFlags, nullptr);
						toPlay.SetVolume(0.9f);
						dialogues->Append(&toPlay);
						ThisCall<void>(0x61F170, topicInfo, 1, character);
					} while (DialogueItemNextResponse(currentItem));
				}
			}
			ThisCall<void>(0x83B8D0, pConversation); //~Conversation
			Engine::GameHeapFree(pConversation);
			ThisCall<void>(0x8D2060, character); //~Character
			Engine::GameHeapFree(character);
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
			SetInDialogueOrHolotapePlaying(true);
			BSWin32Audio::GetSingleton()->FadeInDialogueSound();
		}
	}

	static UInt32 GetMaxLines() {
		if (g_maxLines > 0) {
			if (g_maxLines < 8) return 8;
			if (g_maxLines > 48) return 48;
			return g_maxLines;
		}
		UInt32 screenHeight = GetGameScreenHeight();
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

		auto* mapView = MapMenuAsView(mapMenu);
		ListBoxItem* firstData = static_cast<ListBoxItem*>(mapView->noteList.headData);
		ListNode* nextNode = static_cast<ListNode*>(mapView->noteList.headNext);
		Tile** selectedPtr = reinterpret_cast<Tile**>(&mapView->noteList.selected);
		BGSNote** currentNotePtr = &mapView->currentNote;
		Tile* dataPanelTile = static_cast<Tile*>(mapView->dataPanelTile);

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
		void* mapMenu = GetMapMenu();
		if (!mapMenu) return;
		auto* mapView = MapMenuAsView(mapMenu);
		void* tiles17 = mapView->tabLineTile;
		if (!tiles17) return;
		UInt32 traitID = GetMapMenuCurrentTabTrait();
		if (traitID == 0 || traitID == 0xFFFFFFFF)
			traitID = Engine::Tile_TextToTrait("_CurrentTab");
		ThisCall<void>(0x700320, tiles17, traitID, 3);
		mapView->currentTab = 0x23; //misc
		((void(__cdecl*)())0x79ABA0)();

		if (g_noteToSelect) {
			SelectNoteInList(mapMenu, g_noteToSelect);
			g_noteToSelect = nullptr;
		}
	}

	static void OpenPipBoyToNotes() {
		void* im = GetInterfaceManager();
		if (im) {
			ThisCall<void>(0x70F4E0, im, nullptr, 0x3FF);
			g_switchToMiscPending = true;
		}
	}

	static void ShowNoteMenu(BGSNote* note) {
		if (!note) return;
		UInt8 noteType = BGSNoteGetType(note);
		if (noteType == kBGSNoteType_Text) {
			void* noteText = BGSNoteGetTextSource(note);
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
					BGSNoteMarkRead(note);
				}
			}
		} else if (noteType == kBGSNoteType_Sound || noteType == kBGSNoteType_Voice) {
			void* map = GetMapMenu();
			if (map) {
				PlayHolotape(note, true);
				MapMenuAsView(map)->currentNote = note;
				BGSNoteMarkRead(note);
			}
		} else if (noteType == kBGSNoteType_Image) {
			BGSNoteMarkRead(note);
		}
	}

	typedef void(__thiscall* _MessageMenu_HandleClick)(void* menu, SInt32 tileID, Tile* clickedTile);
	static _MessageMenu_HandleClick ChainedHandleClick = nullptr;

	static void HandleTruncatedMessageClick() {
		bool wasOurMessage = g_noteWasTruncated && g_truncatedNote;
		BGSNote* noteToOpen = wasOurMessage ? g_truncatedNote : nullptr;
		if (wasOurMessage) {
			g_noteWasTruncated = false;
			g_truncatedNote = nullptr;
		}
		if (wasOurMessage) {
			void* im = GetInterfaceManager();
			if (im) {
				UInt8 buttonIndex = InterfaceManagerAsView(im)->msgBoxButton;
				if (buttonIndex == 1) {
					g_openPipBoyPending = true;
					g_noteToSelect = noteToOpen;
				}
			}
		}
	}

	void __fastcall MessageMenu_HandleClick_Hook(void* menu, void* edx, SInt32 tileID, Tile* clickedTile) {
		if (ChainedHandleClick)
			ChainedHandleClick(menu, tileID, clickedTile);
		HandleTruncatedMessageClick();
	}

	static void OnMessageMenuHandleClick(UIMinimal::MessageMenu* menu, SInt32 tileID, UIMinimal::Tile* clickedTile)
	{
		(void)menu;
		(void)tileID;
		(void)clickedTile;
		HandleTruncatedMessageClick();
	}

	void __cdecl OnNoteAddedCallback(BGSNote* note) {
		if (note) {
			g_pendingNote = note;
			g_pendingNoteType = BGSNoteGetType(note);
			g_noteAddedTime = GetTickCount();
			g_controlWasPressed = false;
		}
	}

	static Detours::CallDetour s_noteAddedCall;

	const char* __fastcall ProcessNoteAdded(void* setting, BGSNote* note) {
		OnNoteAddedCallback(note);
		auto original = reinterpret_cast<const char*(__thiscall*)(void*)>(s_noteAddedCall.GetOverwrittenAddr());
		return original ? original(setting) : "";
	}

	__declspec(naked) void OnNoteAddedHook() {
		__asm {
			mov edx, [ebp+8] //BGSNote* arg from caller frame -> fastcall arg2
			jmp ProcessNoteAdded //ecx (setting) already loaded by original call site
		}
	}

	static Detours::CallDetour s_queueUIMessageCall;
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
			const char* controlName = (g_controlID >= 0 && g_controlID <= 27) ? kControlNames[g_controlID] : "Key";
			const char* action = (g_pendingNoteType == 0 || g_pendingNoteType == 3) ? "listen" : "view";
			snprintf(s_modifiedMessage, sizeof(s_modifiedMessage), "%s. Press %s to %s.", msg, controlName, action);
			msg = s_modifiedMessage;
		}
		typedef char(__cdecl* QueueUIMessageFn)(char*, UInt32, char*, char*, float, char);
		auto original = reinterpret_cast<QueueUIMessageFn>(s_queueUIMessageCall.GetOverwrittenAddr());
		return original ? original(msg, emotion, imagePath, soundName, time, instantEnd) : 0;
	}

	class OSInputGlobals {
	public:
		bool GetControlState(UInt32 controlCode, UInt8 state) {
			return Engine::OSInputGlobals_GetControlState(this, controlCode, state);
		}
		static OSInputGlobals* GetSingleton() { return (OSInputGlobals*)*g_inputGlobalsPtr; }
	};

	void Init(int timeoutMs, int controlID, int maxLines) {
		g_timeoutMs = timeoutMs;
		g_controlID = controlID;
		g_maxLines = maxLines;

		s_noteAddedCall.WriteRelCall(0x966B0A, OnNoteAddedHook);
		s_queueUIMessageCall.WriteRelCall(0x966B53, OnQueueUIMessageHook);

		if (MessageBoxQuickClose::IsInstalled()) {
			MessageBoxQuickClose::SetHandleClickObserver(OnMessageMenuHandleClick);
		} else {
			UInt32* vtbl = reinterpret_cast<UInt32*>(kVtbl_MessageMenu);
			ChainedHandleClick = reinterpret_cast<_MessageMenu_HandleClick>(vtbl[kOffset_HandleClick / 4]);
			SafeWrite::Write32(kVtbl_MessageMenu + kOffset_HandleClick, reinterpret_cast<UInt32>(MessageMenu_HandleClick_Hook));
		}
	}

	void Update() {
		if (g_openPipBoyPending) {
			g_openPipBoyPending = false;
			OpenPipBoyToNotes();
		}
		if (g_switchToMiscPending && GetMapMenu()) {
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
			if (BGSNoteIsNoteForm(g_pendingNote))
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
