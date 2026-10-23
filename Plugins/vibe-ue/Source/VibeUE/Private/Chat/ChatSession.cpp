// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Chat/ChatSession.h"
#include "Misc/ConfigCacheIni.h"

DEFINE_LOG_CATEGORY(LogChatSession);

namespace
{
	const TCHAR* ChatConfigSection = TEXT("VibeUE.Chat");
	const TCHAR* AutoSaveBeforePythonExecutionKey = TEXT("AutoSaveBeforePythonExecution");
}

bool FChatSession::IsAutoSaveBeforePythonExecutionEnabled()
{
	bool bEnabled = true;
	GConfig->GetBool(ChatConfigSection, AutoSaveBeforePythonExecutionKey, bEnabled, GEditorPerProjectIni);
	return bEnabled;
}

void FChatSession::SetAutoSaveBeforePythonExecutionEnabled(bool bEnabled)
{
	GConfig->SetBool(ChatConfigSection, AutoSaveBeforePythonExecutionKey, bEnabled, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}
