#pragma once

namespace VATSSpeechFix {
	void Init(bool enabled);
	void SetEnabled(bool enabled);

	//Stewie's audio tweak replaces the vanilla timescale conversion at 0xAEDFBD with this
	//14-byte fldz/fpu-nop sequence, shared here as the one source of truth for the signature
	inline constexpr unsigned char kStewieTimescalePatch[14] = {
		0xD9, 0xE1, 0x66, 0x66, 0x66, 0x66, 0x0F,
		0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00
	};
}
