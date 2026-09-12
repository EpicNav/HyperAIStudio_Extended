// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioTerminalRawInput.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTerminalRawInputTest,
	"HyperAIStudio.Chat.TerminalRawInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioTerminalRawInputTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::TerminalRawInput;

	// Compared as hex so a failure prints the exact wire bytes.
	auto Hex = [](const TArray<uint8>& Bytes) { return BytesToHex(Bytes.GetData(), Bytes.Num()); };
	auto Bytes = [](std::initializer_list<uint8> List) { return TArray<uint8>(List); };
	const TArray<uint8> Open = Bytes({ 0x1B, '[', '2', '0', '0', '~' });
	const TArray<uint8> Close = Bytes({ 0x1B, '[', '2', '0', '1', '~' });

	// Shift+Enter must be ESC and CR together; split, the agent TUI reads a bare Escape and clears input.
	TestEqual(TEXT("Newline insert is exactly ESC CR"), Hex(BuildNewlineInsertPayload()), Hex(Bytes({ 0x1B, 0x0D })));

	TArray<uint8> Expected = Open;
	Expected.Append(Bytes({ 'a', '\n', 'b' }));
	Expected.Append(Close);
	Expected.Add(0x0D);
	TestEqual(TEXT("Bracketed submit wraps the text and puts CR after the closing bracket"),
		Hex(BuildPastePayload(TEXT("a\nb"), true, true)), Hex(Expected));

	Expected = Open;
	Expected.Append(Bytes({ 'a', '\n', 'b' }));
	Expected.Append(Close);
	TestEqual(TEXT("Bracketed insert omits the CR"), Hex(BuildPastePayload(TEXT("a\nb"), true, false)), Hex(Expected));

	TestEqual(TEXT("Non-bracketed mode sends no wrappers"),
		Hex(BuildPastePayload(TEXT("ls"), false, true)), Hex(Bytes({ 'l', 's', 0x0D })));

	TestEqual(TEXT("CRLF and lone CR normalise to LF, matching STerminal's manual paste"),
		Hex(BuildPastePayload(TEXT("a\r\nb\rc"), false, false)), Hex(Bytes({ 'a', '\n', 'b', '\n', 'c' })));

	// U+00E9 is two bytes in UTF-8; a UTF-16 or Latin-1 encoding would be wrong on the wire.
	TestEqual(TEXT("Text is encoded as UTF-8"),
		Hex(BuildPastePayload(TEXT("\u00E9"), false, false)), Hex(Bytes({ 0xC3, 0xA9 })));

	TestEqual(TEXT("Empty non-bracketed insert is empty"), BuildPastePayload(FString(), false, false).Num(), 0);

	// A pasted ESC[201~ must not close the bracket early and turn the rest into typed keys.
	Expected = Open;
	Expected.Append(Bytes({ '[', '2', '0', '1', '~', 'x', '\t', 'y' }));
	Expected.Append(Close);
	TestEqual(TEXT("ESC, DEL and other control characters are stripped; tab survives"),
		Hex(BuildPastePayload(TEXT("\x1b[201~x\ty\x7f\x08"), true, false)), Hex(Expected));

	TestTrue(TEXT("Raw access is compiled in for UE 5.8"), IsAvailable());
	return true;
}

#endif
