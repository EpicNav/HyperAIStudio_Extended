// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

class STerminal;

/**
 * Flip to 0 when a future engine removes STerminal's `friend class FSTerminalSpec` declaration.
 * With 0, every write returns false: the composer falls back to STerminal::ExecuteCommand for
 * single-line submits and Shift+Enter inside the terminal degrades to a plain Enter.
 */
#ifndef HYPERAI_TERMINAL_RAW_INPUT
#define HYPERAI_TERMINAL_RAW_INPUT 1
#endif

/**
 * The only plugin surface that writes raw bytes to an embedded STerminal's PTY.
 * STerminal publishes no raw-write API; see HyperAIStudioTerminalRawInput.cpp for how access is
 * obtained and the invariants that keep it safe.
 */
namespace HyperAIStudio::TerminalRawInput
{
	/** ESC + CR: the xterm meta-Return that Codex, Claude Code and Gemini bind to "insert newline". */
	TArray<uint8> BuildNewlineInsertPayload();

	/**
	 * UTF-8 text with CRLF/CR normalised to LF, wrapped in ESC[200~ / ESC[201~ when bracketed paste is on.
	 * Control characters other than tab and newline are dropped: an embedded ESC[201~ would otherwise end the paste
	 * early and turn the remainder into typed keystrokes. The optional CR is appended after the closing bracket so
	 * the agent reads it as Enter, not pasted text.
	 */
	TArray<uint8> BuildPastePayload(const FString& Text, bool bBracketedPaste, bool bAppendCarriageReturn);

	/** False when raw access is compiled out (HYPERAI_TERMINAL_RAW_INPUT == 0). */
	bool IsAvailable();

	bool HasLiveSession(const STerminal& Terminal);
	bool IsBracketedPasteEnabled(const STerminal& Terminal);

	/** Writes Bytes in a single ITerminalSession::WriteInput call. False if there is no session. */
	bool WriteRawBytes(STerminal& Terminal, TConstArrayView<uint8> Bytes);

	/** Builds a paste payload using the terminal's live bracketed-paste mode and writes it in one call. */
	bool WriteText(STerminal& Terminal, const FString& Text, bool bAppendCarriageReturn);
}
