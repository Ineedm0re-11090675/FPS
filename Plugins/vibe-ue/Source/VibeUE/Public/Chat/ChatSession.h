// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

DECLARE_LOG_CATEGORY_EXTERN(LogChatSession, Log, All);

/**
 * Legacy compatibility shim for the removed in-editor chat system.
 * Only retains local config helpers still used by MCP tool execution.
 */
class VIBEUE_API FChatSession
{
public:
	static bool IsAutoSaveBeforePythonExecutionEnabled();
	static void SetAutoSaveBeforePythonExecutionEnabled(bool bEnabled);
};
