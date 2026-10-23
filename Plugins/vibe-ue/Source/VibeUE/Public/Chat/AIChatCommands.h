// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "Framework/Commands/UICommandList.h"

class SDockTab;
class SWidget;
class ILevelEditor;
class FSpawnTabArgs;

/**
 * Commands for the MCP dashboard
 */
class VIBEUE_API FAIChatCommands : public TCommands<FAIChatCommands>
{
public:
    FAIChatCommands();
    
    /** Register all commands */
    virtual void RegisterCommands() override;
    
    /** Open dashboard window command */
    TSharedPtr<FUICommandInfo> OpenAIChat;
    
    /** Get the command list */
    static TSharedPtr<FUICommandList> GetCommandList() { return CommandList; }
    
    /** Initialize and register with editor */
    static void Initialize();
    
    /** Shutdown and unregister */
    static void Shutdown();
    
    /** Tab name for the dashboard */
    static const FName AIChatTabName;
    
private:
	    /** Register menu extensions */
	    static void RegisterMenus();
	    
	    /** Unregister menu extensions */
	    static void UnregisterMenus();
	    
	    /** Register the nomad tab spawner */
	    static void RegisterTabSpawner();
	    
	    /** Unregister the nomad tab spawner */
	    static void UnregisterTabSpawner();
	    
	    /** Register the status bar drawer */
	    static void RegisterStatusBarPanelDrawer();
	    
	    /** Unregister the status bar drawer */
	    static void UnregisterStatusBarPanelDrawer();
	    
	    /** Level editor created handler (used to register the status bar drawer when it's available) */
	    static void HandleLevelEditorCreated(TSharedPtr<ILevelEditor> InLevelEditor);
	    
	    /** Spawn the dashboard tab */
	    static TSharedRef<SDockTab> SpawnAIChatTab(const FSpawnTabArgs& Args);
	    
	    /** Generate the drawer content for the status bar */
	    static TSharedRef<SWidget> GenerateDrawerContent();
	    
	    /** Handle open dashboard command - toggles drawer/tab */
	    static void HandleOpenAIChat();
	    
	    /** Check if open dashboard command can execute */
	    static bool CanOpenAIChat();
	    
	    /** Command list for the dashboard */
	    static TSharedPtr<FUICommandList> CommandList;
	    
	    /** Menu extension handle */
	    static FDelegateHandle MenuExtensionHandle;
	    
	    /** Level editor created delegate handle */
	    static FDelegateHandle LevelEditorCreatedHandle;
	};
