# HyperAIStudio

HyperAIStudio is a UE 5.8 editor-only plugin that makes Epic's experimental Unreal MCP workflow easier to set up and use with external agent clients.

Core scope:

- Configure and start Epic Unreal MCP on localhost.
- Verify MCP readiness, port listening, generated files, and tool discovery.
- Generate project-local agent instructions and client configs for Codex, Claude Code, Cursor, VS Code/Copilot, and Gemini.
- Copy user-level Codex model-provider TOML for OpenAI-compatible custom providers without storing API key values.
- Create prompt/context packs from selected Blueprint/PCG assets, actors, and Blueprint/PCG graph nodes.
- Open project-root handoff terminals for external agent CLIs and editor agents.

Important limits:

- HyperAIStudio does not ship an in-editor AI chat runtime.
- HyperAIStudio does not proxy MCP traffic or replace Epic Unreal MCP.
- HyperAIStudio does not include Epic `NoRedist` plugin files.
- External agent CLIs and accounts are not bundled.
- Model providers configure the external agent client; they are not MCP servers and are not written into project-local Codex config.

Start from `Tools > HyperAIStudio` or `Tools > HyperAIStudio Quick Action` after enabling the plugin in a UE 5.8 project.

Documentation: https://gamesbyhyper.com/docs/project-foundations/hyper-ai-studio/
