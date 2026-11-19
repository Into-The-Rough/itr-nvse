#pragma once

namespace DialogueCommands {
	void Init(void* nvse);
	void RegisterCommands(void* nvse);
	void RegisterForceSayCommands(void* nvse);
	void RegisterIsSayingCommands(void* nvse);
	void RegisterTopicCommands(void* nvse);
	void RegisterCurrentDialogueCommands(void* nvse);
}
