#pragma once

class Actor;
class TESTopic;

namespace ForceSayCommand {
	bool ForceSay(Actor* speaker, TESTopic* topic, Actor* target);
	bool SayTopic(Actor* speaker, TESTopic* topic, Actor* target);
	void RegisterCommands(void* nvse);
}
