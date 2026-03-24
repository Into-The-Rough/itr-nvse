#pragma once

namespace OnContactHandler {
	bool Init(void* nvseInterface);
	void Update();
	void ClearState();
	void AddWatch(UInt32 refID);
	void RemoveWatch(UInt32 refID);
	bool IsWatched(UInt32 refID);
}
