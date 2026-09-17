// Games by Hyper 2026.

// ============================================================================================
// HAZARD: read before editing this file.
//
// STerminal (Engine/Plugins/Experimental/Terminal) keeps its PTY session and VT parser private
// and offers no raw-write API. It does declare `friend class FSTerminalSpec;` (STerminal.h:26)
// for Epic's own spec test. This file defines a class with that name to obtain the access.
//
// Epic ALSO defines ::FSTerminalSpec, in TerminalTests/Private/Tests/STerminalTests.cpp:95.
// TerminalTests is an Editor/Default module, so both definitions live in the same process.
// That is only safe while every invariant below holds:
//
//   1. Global namespace. The friend declaration names ::FSTerminalSpec; a namespaced class
//      would compile but get no access.
//   2. Plain class. Never BEGIN_DEFINE_SPEC: it would register a second automation test
//      under Epic's name.
//   3. Zero data members, no virtuals, no RTTI. There is no layout or vtable for a linker
//      to fold onto Epic's definition.
//   4. Static member functions only, every name prefixed HyperAIStudio_. The mangled symbols
//      stay disjoint from Epic's even under a monolithic link.
//   5. Defined in this one .cpp, never in a header.
//
// Do not "tidy" this into a namespace, a spec, or a member-holding helper. Do not touch
// STerminal::ScrollOffset or call UpdateScrollBar (private, unexported): the next tick that
// consumes session output repaints the scrollbar itself.
// ============================================================================================

#include "HyperAIStudioTerminalRawInput.h"

#include "Misc/EngineVersionComparison.h"

#if HYPERAI_TERMINAL_RAW_INPUT

#include "ITerminalSession.h"
#include "STerminal.h"

static_assert(ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8,
	"HyperAIStudioTerminalRawInput is pinned to the audited UE 5.8 STerminal layout. Re-check that "
	"STerminal still befriends FSTerminalSpec and still names its members Session and Parser, "
	"then raise this pin, or set HYPERAI_TERMINAL_RAW_INPUT to 0.");

class FSTerminalSpec
{
public:
	static bool HyperAIStudio_HasLiveSession(const STerminal& Terminal)
	{
		return Terminal.Session.IsValid() && Terminal.Session->IsRunning();
	}

	static bool HyperAIStudio_IsBracketedPaste(const STerminal& Terminal)
	{
		return Terminal.Parser.bBracketedPaste;
	}

	static FString HyperAIStudio_ReadVisibleTail(const STerminal& Terminal, const int32 MaxRows)
	{
		const FTerminalBuffer& Buffer = Terminal.Buffer;
		const int32 Columns = Buffer.GetColumns();
		const int32 TotalRows = Buffer.GetTotalRows();
		const int32 Rows = FMath::Clamp(MaxRows, 1, Buffer.GetViewportRows());
		if (Columns <= 0 || TotalRows <= 0)
		{
			return FString();
		}
		const int32 LastRow = TotalRows - 1;
		const int32 FirstRow = FMath::Max(0, LastRow - Rows + 1);
		return Buffer.GetTextInRange(FirstRow, 0, LastRow, Columns - 1);
	}

	static bool HyperAIStudio_WriteRaw(STerminal& Terminal, TConstArrayView<uint8> Bytes)
	{
		if (!Terminal.Session.IsValid() || Bytes.IsEmpty())
		{
			return false;
		}
		Terminal.Session->WriteInput(Bytes);
		return true;
	}
};

#endif // HYPERAI_TERMINAL_RAW_INPUT

namespace HyperAIStudio::TerminalRawInput
{
	TArray<uint8> BuildNewlineInsertPayload()
	{
		return { 0x1B, 0x0D };
	}

	TArray<uint8> BuildPastePayload(const FString& Text, bool bBracketedPaste, bool bAppendCarriageReturn)
	{
		// Mirrors STerminal::HandlePasteShortcut, plus control-character stripping it does not do.
		FString Normalized = Text;
		Normalized.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
		Normalized.ReplaceInline(TEXT("\r"), TEXT("\n"));
		FString Clean;
		Clean.Reserve(Normalized.Len());
		for (const TCHAR Char : Normalized)
		{
			if ((Char >= 0x20 && Char != 0x7F) || Char == TEXT('\t') || Char == TEXT('\n'))
			{
				Clean.AppendChar(Char);
			}
		}
		const FUtf8String Utf8(Clean);

		static const uint8 BracketOpen[] = { 0x1B, '[', '2', '0', '0', '~' };
		static const uint8 BracketClose[] = { 0x1B, '[', '2', '0', '1', '~' };

		TArray<uint8> Bytes;
		Bytes.Reserve(Utf8.Len() + UE_ARRAY_COUNT(BracketOpen) + UE_ARRAY_COUNT(BracketClose) + 1);
		if (bBracketedPaste)
		{
			Bytes.Append(BracketOpen, UE_ARRAY_COUNT(BracketOpen));
		}
		Bytes.Append(reinterpret_cast<const uint8*>(*Utf8), Utf8.Len());
		if (bBracketedPaste)
		{
			Bytes.Append(BracketClose, UE_ARRAY_COUNT(BracketClose));
		}
		if (bAppendCarriageReturn)
		{
			Bytes.Add(0x0D);
		}
		return Bytes;
	}

#if HYPERAI_TERMINAL_RAW_INPUT
	bool IsAvailable()
	{
		return true;
	}

	bool HasLiveSession(const STerminal& Terminal)
	{
		return FSTerminalSpec::HyperAIStudio_HasLiveSession(Terminal);
	}

	bool IsBracketedPasteEnabled(const STerminal& Terminal)
	{
		return FSTerminalSpec::HyperAIStudio_IsBracketedPaste(Terminal);
	}

	bool WriteRawBytes(STerminal& Terminal, TConstArrayView<uint8> Bytes)
	{
		return FSTerminalSpec::HyperAIStudio_WriteRaw(Terminal, Bytes);
	}

	FString ReadVisibleTail(const STerminal& Terminal, const int32 MaxRows)
	{
		return FSTerminalSpec::HyperAIStudio_ReadVisibleTail(Terminal, MaxRows);
	}

	double GetLastOutputTime(const STerminal& Terminal)
	{
		return Terminal.GetLastOutputTime();
	}

	bool WriteText(STerminal& Terminal, const FString& Text, bool bAppendCarriageReturn)
	{
		const TArray<uint8> Bytes = BuildPastePayload(Text, IsBracketedPasteEnabled(Terminal), bAppendCarriageReturn);
		return WriteRawBytes(Terminal, Bytes);
	}
#else
	bool IsAvailable() { return false; }
	bool HasLiveSession(const STerminal&) { return false; }
	bool IsBracketedPasteEnabled(const STerminal&) { return false; }
	bool WriteRawBytes(STerminal&, TConstArrayView<uint8>) { return false; }
	bool WriteText(STerminal&, const FString&, bool) { return false; }
	FString ReadVisibleTail(const STerminal&, int32) { return FString(); }
	double GetLastOutputTime(const STerminal&) { return 0.0; }
#endif
}
