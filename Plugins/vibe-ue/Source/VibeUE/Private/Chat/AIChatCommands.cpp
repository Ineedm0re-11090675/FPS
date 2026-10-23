// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Chat/AIChatCommands.h"
#include "UI/SAIChatWindow.h"
#include "Framework/Docking/TabManager.h"
#include "ToolMenus.h"
#include "Styling/AppStyle.h"
#include "LevelEditor.h"
#include "Editor.h"
#include "Widgets/Docking/SDockTab.h"
#include "StatusBarSubsystem.h"
#include "WidgetDrawerConfig.h"

#define LOCTEXT_NAMESPACE "AIChatCommands"

// Static members
TSharedPtr<FUICommandList> FAIChatCommands::CommandList;
FDelegateHandle FAIChatCommands::MenuExtensionHandle;
FDelegateHandle FAIChatCommands::LevelEditorCreatedHandle;
const FName FAIChatCommands::AIChatTabName("VibeUEAIChat");

FAIChatCommands::FAIChatCommands()
    : TCommands<FAIChatCommands>(
        TEXT("AIChatCommands"),
        LOCTEXT("AIChatCommands", "MCP Dashboard Commands"),
        NAME_None,
        FAppStyle::GetAppStyleSetName())
{
}

void FAIChatCommands::RegisterCommands()
{
    UI_COMMAND(
        OpenAIChat,
        "Open MCP Dashboard",
        "Open the VibeUE MCP Dashboard panel",
        EUserInterfaceActionType::Button,
        FInputChord(EModifierKey::Control | EModifierKey::Shift, EKeys::V)
    );
}

void FAIChatCommands::Initialize()
{
    // Register commands
    FAIChatCommands::Register();
    
    // Create command list
    CommandList = MakeShared<FUICommandList>();
    
    // Map commands to actions
    CommandList->MapAction(
        Get().OpenAIChat,
        FExecuteAction::CreateStatic(&FAIChatCommands::HandleOpenAIChat),
        FCanExecuteAction::CreateStatic(&FAIChatCommands::CanOpenAIChat)
    );
    
    // Bind to Level Editor's global actions so keyboard shortcuts work
    FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
    LevelEditorModule.GetGlobalLevelEditorActions()->Append(CommandList.ToSharedRef());
    
    // Register tab spawner
    RegisterTabSpawner();
    
    // Register menus via startup callback to ensure UToolMenus is fully initialized
    UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateStatic(&FAIChatCommands::RegisterMenus));
    
    // Register status bar drawer once the level editor exists (status bar widget is created during SLevelEditor::Initialize)
    LevelEditorCreatedHandle = LevelEditorModule.OnLevelEditorCreated().AddStatic(&FAIChatCommands::HandleLevelEditorCreated);
    RegisterStatusBarPanelDrawer();
    
    UE_LOG(LogTemp, Log, TEXT("MCP Dashboard commands initialized with panel drawer support"));
}

void FAIChatCommands::Shutdown()
{
    if (LevelEditorCreatedHandle.IsValid() && FModuleManager::Get().IsModuleLoaded("LevelEditor"))
    {
        FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
        LevelEditorModule.OnLevelEditorCreated().Remove(LevelEditorCreatedHandle);
        LevelEditorCreatedHandle.Reset();
    }

    UnregisterStatusBarPanelDrawer();
    UnregisterTabSpawner();
    UnregisterMenus();
    
    CommandList.Reset();
    
    FAIChatCommands::Unregister();
    
    UE_LOG(LogTemp, Log, TEXT("MCP Dashboard commands shutdown"));
}

void FAIChatCommands::RegisterTabSpawner()
{
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        AIChatTabName,
        FOnSpawnTab::CreateStatic(&FAIChatCommands::SpawnAIChatTab))
        .SetDisplayName(LOCTEXT("AIChatTabTitle", "VibeUE MCP Dashboard"))
        .SetMenuType(ETabSpawnerMenuType::Hidden)
        .SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Comment"))
        .SetCanSidebarTab(false);  // Panel drawer tabs don't work well as sidebar tabs
    
    UE_LOG(LogTemp, Log, TEXT("MCP Dashboard tab spawner registered"));
}

void FAIChatCommands::UnregisterTabSpawner()
{
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(AIChatTabName);
}

void FAIChatCommands::RegisterStatusBarPanelDrawer()
{
    if (GEditor)
    {
        // Make sure StatusBar module is loaded
        FModuleManager::Get().LoadModuleChecked(TEXT("StatusBar"));
        
        if (UStatusBarSubsystem* StatusBarSubsystem = GEditor->GetEditorSubsystem<UStatusBarSubsystem>())
        {
            static const FName LevelEditorStatusBarName(TEXT("LevelEditor.StatusBar"));

            FWidgetDrawerConfig DrawerConfig(AIChatTabName);
            DrawerConfig.GetDrawerContentDelegate = FOnGetContent::CreateStatic(&FAIChatCommands::GenerateDrawerContent);
            DrawerConfig.ButtonText = LOCTEXT("StatusBarVibeUE", "VibeUE");
            DrawerConfig.ToolTipText = LOCTEXT("StatusBarVibeUETooltip", "Open the VibeUE MCP Dashboard drawer");
            DrawerConfig.Icon = FAppStyle::GetBrush(TEXT("Icons.Comment"));

            StatusBarSubsystem->RegisterDrawer(LevelEditorStatusBarName, MoveTemp(DrawerConfig));

            UE_LOG(LogTemp, Log, TEXT("MCP Dashboard drawer registered in status bar"));
        }
    }
}

void FAIChatCommands::UnregisterStatusBarPanelDrawer()
{
    if (GEditor)
    {
        if (UStatusBarSubsystem* StatusBarSubsystem = GEditor->GetEditorSubsystem<UStatusBarSubsystem>())
        {
            static const FName LevelEditorStatusBarName(TEXT("LevelEditor.StatusBar"));
            StatusBarSubsystem->UnregisterDrawer(LevelEditorStatusBarName, AIChatTabName);
        }
    }
}

TSharedRef<SDockTab> FAIChatCommands::SpawnAIChatTab(const FSpawnTabArgs& Args)
{
    return SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        .Label(LOCTEXT("AIChatTabLabel", "VibeUE MCP Dashboard"))
        [
            SNew(SAIChatWindow)
        ];
}

void FAIChatCommands::HandleLevelEditorCreated(TSharedPtr<ILevelEditor> InLevelEditor)
{
    RegisterStatusBarPanelDrawer();
}

TSharedRef<SWidget> FAIChatCommands::GenerateDrawerContent()
{
    return SNew(SAIChatWindow);
}

void FAIChatCommands::RegisterMenus()
{
    UToolMenus* ToolMenus = UToolMenus::Get();
    if (!ToolMenus)
    {
        return;
    }

    // Owner will be used for cleanup in call to UToolMenus::UnregisterOwner()
    FToolMenuOwnerScoped OwnerScoped("AIChatCommands");

    // Add to Window menu under Assistance section (alongside Epic AI Assistant)
    {
        UToolMenu* WindowMenu = ToolMenus->ExtendMenu("MainFrame.MainMenu.Window");

        FToolMenuSection& Section = WindowMenu->FindOrAddSection("Assistance");

        Section.AddEntry(FToolMenuEntry::InitMenuEntry(
            "VibeUEAIChat",
            LOCTEXT("OpenAIChatLabel", "VibeUE MCP Dashboard"),
            LOCTEXT("OpenAIChatTooltip", "Open the VibeUE MCP Dashboard panel (Ctrl+Shift+V)"),
            FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Comment"),
            FUIAction(
                FExecuteAction::CreateStatic(&FAIChatCommands::HandleOpenAIChat),
                FCanExecuteAction::CreateStatic(&FAIChatCommands::CanOpenAIChat)
            )
        ));
    }

    ToolMenus->RefreshAllWidgets();
}

void FAIChatCommands::UnregisterMenus()
{
    // Use UnregisterOwner to remove only our entries, not the entire section
    // (the Assistance section is shared with Epic's AI Assistant)
    UToolMenus::UnregisterOwner("AIChatCommands");
}

void FAIChatCommands::HandleOpenAIChat()
{
    // Prefer the status bar drawer if registered, otherwise fall back to a normal nomad tab.
    if (GEditor)
    {
        RegisterStatusBarPanelDrawer();
        if (UStatusBarSubsystem* StatusBarSubsystem = GEditor->GetEditorSubsystem<UStatusBarSubsystem>())
        {
            if (StatusBarSubsystem->TryToggleDrawer(AIChatTabName))
            {
                return;
            }
        }
    }

    // Fallback: just invoke the tab normally
    FGlobalTabmanager::Get()->TryInvokeTab(AIChatTabName);
}

bool FAIChatCommands::CanOpenAIChat()
{
    return true;
}

#undef LOCTEXT_NAMESPACE
