// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Core/ToolRegistry.h"
#include "MCP/MCPServer.h"
#include "PythonAPI/UActorService.h"
#include "PythonAPI/UAssetDiscoveryService.h"
#include "Tools/PythonTools.h"
#include "Utils/VibeUEPaths.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "GameMapsSettings.h"
#include "GeneralProjectSettings.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProperties.h"
#include "Json.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "WorldPartition/WorldPartition.h"

namespace
{
	static FString ExtractParamFromJson(const TMap<FString, FString>& Params, const FString& FieldName)
	{
		if (const FString* DirectParam = Params.Find(FieldName))
		{
			return *DirectParam;
		}

		FString CapitalizedField = FieldName;
		if (CapitalizedField.Len() > 0)
		{
			CapitalizedField[0] = FChar::ToUpper(CapitalizedField[0]);
		}

		if (const FString* CapitalizedParam = Params.Find(CapitalizedField))
		{
			return *CapitalizedParam;
		}

		const FString* ParamsJsonStr = Params.Find(TEXT("ParamsJson"));
		if (!ParamsJsonStr)
		{
			return FString();
		}

		TSharedPtr<FJsonObject> JsonObj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ParamsJsonStr);
		if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
		{
			return FString();
		}

		FString Value;
		if (JsonObj->TryGetStringField(FieldName, Value))
		{
			return Value;
		}

		return FString();
	}

	static bool ExtractBoolParam(const TMap<FString, FString>& Params, const FString& FieldName, bool bDefaultValue)
	{
		const FString Value = ExtractParamFromJson(Params, FieldName);
		if (Value.IsEmpty())
		{
			return bDefaultValue;
		}

		return Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("1"));
	}

	static int32 ExtractIntParam(const TMap<FString, FString>& Params, const FString& FieldName, int32 DefaultValue, int32 MinValue = 1, int32 MaxValue = 200)
	{
		const FString Value = ExtractParamFromJson(Params, FieldName);
		if (Value.IsEmpty())
		{
			return FMath::Clamp(DefaultValue, MinValue, MaxValue);
		}

		return FMath::Clamp(FCString::Atoi(*Value), MinValue, MaxValue);
	}

	static FString SerializeJsonObject(const TSharedPtr<FJsonObject>& JsonObject)
	{
		FString JsonString;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
		FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
		return JsonString;
	}

	static TSharedPtr<FJsonObject> BuildErrorResponse(const FString& ErrorCode, const FString& ErrorMessage, const FString& SuggestedNextStep = TEXT(""))
	{
		const TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
		ErrorObj->SetBoolField(TEXT("success"), false);
		ErrorObj->SetStringField(TEXT("error_code"), ErrorCode);
		ErrorObj->SetStringField(TEXT("error_message"), ErrorMessage);

		if (!SuggestedNextStep.IsEmpty())
		{
			ErrorObj->SetStringField(TEXT("suggested_next_step"), SuggestedNextStep);
		}

		return ErrorObj;
	}

	static IAssetRegistry* GetAssetRegistry()
	{
		return &FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	}

	static TSharedPtr<FJsonObject> BuildAssetJson(const FAssetData& AssetData)
	{
		const TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
		AssetObj->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		AssetObj->SetStringField(TEXT("asset_class"), AssetData.AssetClassPath.GetAssetName().ToString());
		AssetObj->SetStringField(TEXT("asset_class_path"), AssetData.AssetClassPath.ToString());
		AssetObj->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
		AssetObj->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
		AssetObj->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
		AssetObj->SetBoolField(TEXT("is_valid"), AssetData.IsValid());
		return AssetObj;
	}

	static TSharedPtr<FJsonObject> BuildLevelActorJson(const FLevelActorInfo& ActorInfo)
	{
		const TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
		ActorObj->SetStringField(TEXT("actor_name"), ActorInfo.ActorName);
		ActorObj->SetStringField(TEXT("actor_label"), ActorInfo.ActorLabel);
		ActorObj->SetStringField(TEXT("actor_class"), ActorInfo.ActorClass);
		ActorObj->SetStringField(TEXT("folder_path"), ActorInfo.FolderPath);
		ActorObj->SetBoolField(TEXT("is_hidden"), ActorInfo.bIsHidden);
		ActorObj->SetBoolField(TEXT("is_selected"), ActorInfo.bIsSelected);

		const TSharedPtr<FJsonObject> LocationObj = MakeShared<FJsonObject>();
		LocationObj->SetNumberField(TEXT("x"), ActorInfo.Location.X);
		LocationObj->SetNumberField(TEXT("y"), ActorInfo.Location.Y);
		LocationObj->SetNumberField(TEXT("z"), ActorInfo.Location.Z);
		ActorObj->SetObjectField(TEXT("location"), LocationObj);

		const TSharedPtr<FJsonObject> RotationObj = MakeShared<FJsonObject>();
		RotationObj->SetNumberField(TEXT("pitch"), ActorInfo.Rotation.Pitch);
		RotationObj->SetNumberField(TEXT("yaw"), ActorInfo.Rotation.Yaw);
		RotationObj->SetNumberField(TEXT("roll"), ActorInfo.Rotation.Roll);
		ActorObj->SetObjectField(TEXT("rotation"), RotationObj);

		const TSharedPtr<FJsonObject> ScaleObj = MakeShared<FJsonObject>();
		ScaleObj->SetNumberField(TEXT("x"), ActorInfo.Scale.X);
		ScaleObj->SetNumberField(TEXT("y"), ActorInfo.Scale.Y);
		ScaleObj->SetNumberField(TEXT("z"), ActorInfo.Scale.Z);
		ActorObj->SetObjectField(TEXT("scale"), ScaleObj);

		return ActorObj;
	}

	static FString GetWorldTypeLabel(EWorldType::Type WorldType)
	{
		switch (WorldType)
		{
		case EWorldType::None:
			return TEXT("None");
		case EWorldType::Game:
			return TEXT("Game");
		case EWorldType::Editor:
			return TEXT("Editor");
		case EWorldType::PIE:
			return TEXT("PIE");
		case EWorldType::EditorPreview:
			return TEXT("EditorPreview");
		case EWorldType::GamePreview:
			return TEXT("GamePreview");
		case EWorldType::GameRPC:
			return TEXT("GameRPC");
		case EWorldType::Inactive:
			return TEXT("Inactive");
		default:
			return TEXT("Unknown");
		}
	}

	static bool IsWorldMutationReady(UWorld* World, FString& OutReason)
	{
		if (!GEditor)
		{
			OutReason = TEXT("Editor is not available yet.");
			return false;
		}

		if (!World)
		{
			OutReason = TEXT("Editor world is not available yet.");
			return false;
		}

		if (UWorldPartition* WorldPartition = World->GetWorldPartition())
		{
			if (!WorldPartition->IsInitialized())
			{
				OutReason = TEXT("World Partition is not initialized yet.");
				return false;
			}

			if (!WorldPartition->GetResolvingDataLayerManager())
			{
				OutReason = TEXT("World Partition data layers are still initializing.");
				return false;
			}
		}

		OutReason = TEXT("Editor world is ready for read/write tooling.");
		return true;
	}

	static bool TryGetEditorWorld(UWorld*& OutWorld, FString& OutErrorCode, FString& OutErrorMessage, FString& OutSuggestedNextStep)
	{
		if (!GEditor)
		{
			OutErrorCode = TEXT("EDITOR_UNAVAILABLE");
			OutErrorMessage = TEXT("Unreal Editor is not available yet.");
			OutSuggestedNextStep = TEXT("Wait for the editor to finish loading, then retry.");
			OutWorld = nullptr;
			return false;
		}

		OutWorld = GEditor->GetEditorWorldContext().World();
		if (!OutWorld)
		{
			OutErrorCode = TEXT("EDITOR_WORLD_UNAVAILABLE");
			OutErrorMessage = TEXT("Editor world is not available yet.");
			OutSuggestedNextStep = TEXT("Open a level in the editor or wait for the current level to finish loading.");
			return false;
		}

		return true;
	}

	static int32 CountActors(UWorld* World)
	{
		int32 Count = 0;
		if (!World)
		{
			return Count;
		}

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			++Count;
		}

		return Count;
	}

	static int32 CountSelectedActors(UWorld* World)
	{
		int32 Count = 0;
		if (!World)
		{
			return Count;
		}

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (AActor* Actor = *It; Actor && Actor->IsSelected())
			{
				++Count;
			}
		}

		return Count;
	}

	static void CountToolCallStates(const TArray<FMCPToolCallRecord>& Records, int32& OutRunning, int32& OutSucceeded, int32& OutFailed)
	{
		OutRunning = 0;
		OutSucceeded = 0;
		OutFailed = 0;

		for (const FMCPToolCallRecord& Record : Records)
		{
			if (Record.bInProgress)
			{
				++OutRunning;
			}
			else if (Record.bIsError)
			{
				++OutFailed;
			}
			else
			{
				++OutSucceeded;
			}
		}
	}

	static int32 GetActiveClientCount(const FMCPServerDiagnosticsSnapshot& Snapshot)
	{
		return FMath::Max(Snapshot.ActiveSSEConnections, Snapshot.ActiveSessions);
	}

	static TArray<FString> GetContextToolNames()
	{
		return {
			TEXT("context_overview"),
			TEXT("editor_context"),
			TEXT("project_info"),
			TEXT("current_level"),
			TEXT("selected_assets"),
			TEXT("open_editors"),
			TEXT("self_test")
		};
	}

	static TSharedPtr<FJsonObject> BuildToolRegistrySummaryJson()
	{
		FToolRegistry& ToolRegistry = FToolRegistry::Get();
		const TArray<FToolMetadata>& AllTools = ToolRegistry.GetAllTools();
		const TArray<FToolMetadata> EnabledTools = ToolRegistry.GetEnabledTools();
		const TArray<FString> RequiredToolNames = GetContextToolNames();

		const TSharedPtr<FJsonObject> ToolSummaryObj = MakeShared<FJsonObject>();
		ToolSummaryObj->SetBoolField(TEXT("initialized"), ToolRegistry.IsInitialized());
		ToolSummaryObj->SetNumberField(TEXT("registered_count"), AllTools.Num());
		ToolSummaryObj->SetNumberField(TEXT("enabled_count"), EnabledTools.Num());
		ToolSummaryObj->SetNumberField(TEXT("disabled_count"), AllTools.Num() - EnabledTools.Num());

		TArray<TSharedPtr<FJsonValue>> RequiredToolsArray;
		RequiredToolsArray.Reserve(RequiredToolNames.Num());

		for (const FString& ToolName : RequiredToolNames)
		{
			const TSharedPtr<FJsonObject> ToolStateObj = MakeShared<FJsonObject>();
			ToolStateObj->SetStringField(TEXT("name"), ToolName);
			ToolStateObj->SetBoolField(TEXT("registered"), ToolRegistry.FindTool(ToolName) != nullptr);
			ToolStateObj->SetBoolField(TEXT("enabled"), ToolRegistry.IsToolEnabled(ToolName));
			RequiredToolsArray.Add(MakeShared<FJsonValueObject>(ToolStateObj));
		}

		ToolSummaryObj->SetArrayField(TEXT("required_tools"), RequiredToolsArray);
		return ToolSummaryObj;
	}

	static TSharedPtr<FJsonObject> BuildMcpSummaryJson()
	{
		const FMCPServerDiagnosticsSnapshot Snapshot = FMCPServer::Get().GetDiagnosticsSnapshot(20);
		int32 RunningCount = 0;
		int32 SuccessCount = 0;
		int32 FailedCount = 0;
		CountToolCallStates(Snapshot.RecentToolCalls, RunningCount, SuccessCount, FailedCount);

		const TSharedPtr<FJsonObject> MpcObj = MakeShared<FJsonObject>();
		MpcObj->SetBoolField(TEXT("server_running"), Snapshot.bIsRunning);
		MpcObj->SetStringField(TEXT("server_url"), Snapshot.ServerUrl);
		MpcObj->SetNumberField(TEXT("port"), Snapshot.Port);
		MpcObj->SetNumberField(TEXT("active_clients"), GetActiveClientCount(Snapshot));
		MpcObj->SetNumberField(TEXT("active_sessions"), Snapshot.ActiveSessions);
		MpcObj->SetNumberField(TEXT("active_sse_connections"), Snapshot.ActiveSSEConnections);
		MpcObj->SetNumberField(TEXT("accepted_connections"), Snapshot.TotalConnectionsAccepted);
		MpcObj->SetNumberField(TEXT("rpc_request_count"), Snapshot.TotalRpcRequests);
		MpcObj->SetStringField(TEXT("last_rpc_method"), Snapshot.LastRpcMethod);
		MpcObj->SetStringField(TEXT("last_rpc_id"), Snapshot.LastRpcId);
		MpcObj->SetStringField(TEXT("last_tool_call"), Snapshot.LastToolsCallToolName);
		MpcObj->SetBoolField(TEXT("last_tool_call_is_error"), Snapshot.bLastToolsCallIsError);
		MpcObj->SetStringField(TEXT("last_tool_call_error"), Snapshot.LastToolsCallErrorSummary);

		const TSharedPtr<FJsonObject> ToolCallSummaryObj = MakeShared<FJsonObject>();
		ToolCallSummaryObj->SetNumberField(TEXT("running"), RunningCount);
		ToolCallSummaryObj->SetNumberField(TEXT("succeeded"), SuccessCount);
		ToolCallSummaryObj->SetNumberField(TEXT("failed"), FailedCount);
		MpcObj->SetObjectField(TEXT("tool_call_summary"), ToolCallSummaryObj);

		if (Snapshot.LastConnectionAcceptedUtc.GetTicks() > 0)
		{
			MpcObj->SetStringField(TEXT("last_connection_accepted_utc"), Snapshot.LastConnectionAcceptedUtc.ToIso8601());
		}

		if (Snapshot.LastRpcUtc.GetTicks() > 0)
		{
			MpcObj->SetStringField(TEXT("last_rpc_utc"), Snapshot.LastRpcUtc.ToIso8601());
		}

		if (Snapshot.LastToolsCallUtc.GetTicks() > 0)
		{
			MpcObj->SetStringField(TEXT("last_tool_call_utc"), Snapshot.LastToolsCallUtc.ToIso8601());
		}

		return MpcObj;
	}

	static TSharedPtr<FJsonObject> BuildProjectInfoJson()
	{
		const TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
		RootObj->SetBoolField(TEXT("success"), true);
		RootObj->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());
		RootObj->SetStringField(TEXT("project_name"), FApp::GetProjectName());

		FString ProjectFilePath = FPaths::GetProjectFilePath();
		if (ProjectFilePath.IsEmpty())
		{
			ProjectFilePath = FPaths::ProjectDir() / FString::Printf(TEXT("%s.uproject"), *FApp::GetProjectName());
		}

		RootObj->SetStringField(TEXT("project_file_path"), FPaths::ConvertRelativePathToFull(ProjectFilePath));
		RootObj->SetStringField(TEXT("project_dir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		RootObj->SetStringField(TEXT("content_dir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()));
		RootObj->SetStringField(TEXT("config_dir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir()));
		RootObj->SetStringField(TEXT("saved_dir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()));
		RootObj->SetStringField(TEXT("plugins_dir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectPluginsDir()));

		const FEngineVersion EngineVersion = FEngineVersion::Current();
		const TSharedPtr<FJsonObject> EngineObj = MakeShared<FJsonObject>();
		EngineObj->SetStringField(TEXT("version"), EngineVersion.ToString());
		EngineObj->SetStringField(TEXT("branch"), EngineVersion.GetBranch());
		EngineObj->SetNumberField(TEXT("major"), EngineVersion.GetMajor());
		EngineObj->SetNumberField(TEXT("minor"), EngineVersion.GetMinor());
		EngineObj->SetNumberField(TEXT("patch"), EngineVersion.GetPatch());
		EngineObj->SetStringField(TEXT("platform_name"), FPlatformProperties::PlatformName());
		EngineObj->SetStringField(TEXT("engine_dir"), FPaths::ConvertRelativePathToFull(FPaths::EngineDir()));
		RootObj->SetObjectField(TEXT("engine"), EngineObj);

		const TSharedPtr<FJsonObject> PluginObj = MakeShared<FJsonObject>();
		PluginObj->SetStringField(TEXT("plugin_dir"), FVibeUEPaths::GetPluginDir());
		PluginObj->SetStringField(TEXT("plugin_content_dir"), FVibeUEPaths::GetPluginContentDir());
		PluginObj->SetStringField(TEXT("plugin_config_dir"), FVibeUEPaths::GetConfigDir());
		PluginObj->SetStringField(TEXT("instructions_dir"), FVibeUEPaths::GetInstructionsDir());
		RootObj->SetObjectField(TEXT("vibeue_plugin"), PluginObj);

		const UGeneralProjectSettings* ProjectSettings = GetDefault<UGeneralProjectSettings>();
		const TSharedPtr<FJsonObject> ProjectSettingsObj = MakeShared<FJsonObject>();
		if (ProjectSettings)
		{
			ProjectSettingsObj->SetStringField(TEXT("project_name_override"), ProjectSettings->ProjectName);
			ProjectSettingsObj->SetStringField(TEXT("project_version"), ProjectSettings->ProjectVersion);
			ProjectSettingsObj->SetStringField(TEXT("displayed_title"), ProjectSettings->ProjectDisplayedTitle.ToString());
			ProjectSettingsObj->SetStringField(TEXT("description"), ProjectSettings->Description);
			ProjectSettingsObj->SetStringField(TEXT("company_name"), ProjectSettings->CompanyName);
			ProjectSettingsObj->SetStringField(TEXT("homepage"), ProjectSettings->Homepage);
			ProjectSettingsObj->SetStringField(TEXT("support_contact"), ProjectSettings->SupportContact);
			ProjectSettingsObj->SetStringField(TEXT("copyright_notice"), ProjectSettings->CopyrightNotice);
			ProjectSettingsObj->SetStringField(TEXT("project_id"), ProjectSettings->ProjectID.ToString(EGuidFormats::DigitsWithHyphens));
		}
		RootObj->SetObjectField(TEXT("project_settings"), ProjectSettingsObj);

		const UGameMapsSettings* MapSettings = GetDefault<UGameMapsSettings>();
		const TSharedPtr<FJsonObject> MapSettingsObj = MakeShared<FJsonObject>();
		MapSettingsObj->SetStringField(TEXT("game_default_map"), UGameMapsSettings::GetGameDefaultMap(EDefaultMapRequestType::Client));
		MapSettingsObj->SetStringField(TEXT("server_default_map"), UGameMapsSettings::GetGameDefaultMap(EDefaultMapRequestType::Server));
#if WITH_EDITORONLY_DATA
		if (MapSettings)
		{
			MapSettingsObj->SetStringField(TEXT("editor_startup_map"), MapSettings->EditorStartupMap.ToString());
		}
		else
		{
			MapSettingsObj->SetStringField(TEXT("editor_startup_map"), TEXT(""));
		}
#endif
		MapSettingsObj->SetStringField(TEXT("transition_map"), MapSettings ? MapSettings->TransitionMap.ToString() : TEXT(""));
		MapSettingsObj->SetStringField(TEXT("global_default_game_mode"), UGameMapsSettings::GetGlobalDefaultGameMode());
		RootObj->SetObjectField(TEXT("maps"), MapSettingsObj);

		return RootObj;
	}

	static TSharedPtr<FJsonObject> BuildCurrentLevelJson(bool bIncludeActorSample, int32 MaxActorSample)
	{
		UWorld* World = nullptr;
		FString ErrorCode;
		FString ErrorMessage;
		FString SuggestedNextStep;
		if (!TryGetEditorWorld(World, ErrorCode, ErrorMessage, SuggestedNextStep))
		{
			return BuildErrorResponse(ErrorCode, ErrorMessage, SuggestedNextStep);
		}

		const TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
		RootObj->SetBoolField(TEXT("success"), true);
		RootObj->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());
		RootObj->SetStringField(TEXT("world_name"), World->GetName());
		RootObj->SetStringField(TEXT("map_name"), World->GetMapName());
		RootObj->SetStringField(TEXT("world_type"), GetWorldTypeLabel(World->WorldType));
		RootObj->SetStringField(TEXT("world_object_path"), World->GetPathName());

		if (UPackage* WorldPackage = World->GetOutermost())
		{
			RootObj->SetStringField(TEXT("package_name"), WorldPackage->GetName());
			RootObj->SetBoolField(TEXT("is_dirty"), WorldPackage->IsDirty());
		}
		else
		{
			RootObj->SetStringField(TEXT("package_name"), TEXT(""));
			RootObj->SetBoolField(TEXT("is_dirty"), false);
		}

		RootObj->SetNumberField(TEXT("actor_count"), CountActors(World));
		RootObj->SetNumberField(TEXT("selected_actor_count"), CountSelectedActors(World));

		const TSharedPtr<FJsonObject> PersistentLevelObj = MakeShared<FJsonObject>();
		if (ULevel* PersistentLevel = World->PersistentLevel)
		{
			PersistentLevelObj->SetStringField(TEXT("level_name"), PersistentLevel->GetName());
			PersistentLevelObj->SetStringField(TEXT("package_name"), PersistentLevel->GetPackage() ? PersistentLevel->GetPackage()->GetName() : TEXT(""));
			PersistentLevelObj->SetBoolField(TEXT("is_dirty"), PersistentLevel->GetPackage() ? PersistentLevel->GetPackage()->IsDirty() : false);
		}
		RootObj->SetObjectField(TEXT("persistent_level"), PersistentLevelObj);

		int32 LoadedStreamingLevels = 0;
		int32 VisibleStreamingLevels = 0;
		for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
		{
			if (!StreamingLevel)
			{
				continue;
			}

			if (StreamingLevel->IsLevelLoaded())
			{
				++LoadedStreamingLevels;
			}

			if (StreamingLevel->GetShouldBeVisibleFlag())
			{
				++VisibleStreamingLevels;
			}
		}

		const TSharedPtr<FJsonObject> StreamingObj = MakeShared<FJsonObject>();
		StreamingObj->SetNumberField(TEXT("total"), World->GetStreamingLevels().Num());
		StreamingObj->SetNumberField(TEXT("loaded"), LoadedStreamingLevels);
		StreamingObj->SetNumberField(TEXT("visible"), VisibleStreamingLevels);
		RootObj->SetObjectField(TEXT("streaming_levels"), StreamingObj);

		const TSharedPtr<FJsonObject> WorldPartitionObj = MakeShared<FJsonObject>();
		if (UWorldPartition* WorldPartition = World->GetWorldPartition())
		{
			WorldPartitionObj->SetBoolField(TEXT("enabled"), true);
			WorldPartitionObj->SetBoolField(TEXT("initialized"), WorldPartition->IsInitialized());
			WorldPartitionObj->SetBoolField(TEXT("data_layers_ready"), WorldPartition->GetResolvingDataLayerManager() != nullptr);
		}
		else
		{
			WorldPartitionObj->SetBoolField(TEXT("enabled"), false);
			WorldPartitionObj->SetBoolField(TEXT("initialized"), true);
			WorldPartitionObj->SetBoolField(TEXT("data_layers_ready"), true);
		}
		RootObj->SetObjectField(TEXT("world_partition"), WorldPartitionObj);

		FString WorldMutationReadiness;
		const bool bWorldMutationReady = IsWorldMutationReady(World, WorldMutationReadiness);
		RootObj->SetBoolField(TEXT("world_mutation_ready"), bWorldMutationReady);
		RootObj->SetStringField(TEXT("world_mutation_state"), WorldMutationReadiness);

		if (bIncludeActorSample)
		{
			const int32 ClampedMaxActorSample = FMath::Clamp(MaxActorSample, 1, 100);
			const TArray<FLevelActorInfo> ActorSample = UActorService::ListLevelActors(TEXT(""), true, ClampedMaxActorSample);
			TArray<TSharedPtr<FJsonValue>> ActorArray;
			ActorArray.Reserve(ActorSample.Num());

			for (const FLevelActorInfo& ActorInfo : ActorSample)
			{
				ActorArray.Add(MakeShared<FJsonValueObject>(BuildLevelActorJson(ActorInfo)));
			}

			RootObj->SetArrayField(TEXT("actor_sample"), ActorArray);
			RootObj->SetNumberField(TEXT("actor_sample_count"), ActorSample.Num());
			RootObj->SetNumberField(TEXT("actor_sample_limit"), ClampedMaxActorSample);
			RootObj->SetNumberField(TEXT("actor_sample_truncated_count"), FMath::Max(0, CountActors(World) - ActorSample.Num()));
		}

		return RootObj;
	}

	static TSharedPtr<FJsonObject> BuildSelectedAssetsJson(int32 MaxAssets)
	{
		if (!GEditor)
		{
			return BuildErrorResponse(
				TEXT("EDITOR_UNAVAILABLE"),
				TEXT("Unreal Editor is not available yet."),
				TEXT("Wait for the editor to finish loading, then retry.")
			);
		}

		const int32 ClampedMaxAssets = FMath::Clamp(MaxAssets, 1, 100);
		const TArray<FAssetData> SelectedAssets = UAssetDiscoveryService::GetContentBrowserSelections();

		const TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
		RootObj->SetBoolField(TEXT("success"), true);
		RootObj->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());
		RootObj->SetNumberField(TEXT("selection_count"), SelectedAssets.Num());
		RootObj->SetNumberField(TEXT("result_limit"), ClampedMaxAssets);
		RootObj->SetNumberField(TEXT("truncated_count"), FMath::Max(0, SelectedAssets.Num() - ClampedMaxAssets));

		FAssetData PrimaryAsset;
		if (UAssetDiscoveryService::GetPrimaryContentBrowserSelection(PrimaryAsset))
		{
			RootObj->SetObjectField(TEXT("primary_selection"), BuildAssetJson(PrimaryAsset));
		}

		TArray<TSharedPtr<FJsonValue>> AssetArray;
		AssetArray.Reserve(FMath::Min(SelectedAssets.Num(), ClampedMaxAssets));

		for (int32 Index = 0; Index < SelectedAssets.Num() && Index < ClampedMaxAssets; ++Index)
		{
			AssetArray.Add(MakeShared<FJsonValueObject>(BuildAssetJson(SelectedAssets[Index])));
		}

		RootObj->SetArrayField(TEXT("assets"), AssetArray);
		return RootObj;
	}

	static TSharedPtr<FJsonObject> BuildOpenEditorsJson(int32 MaxAssets)
	{
		if (!GEditor)
		{
			return BuildErrorResponse(
				TEXT("EDITOR_UNAVAILABLE"),
				TEXT("Unreal Editor is not available yet."),
				TEXT("Wait for the editor to finish loading, then retry.")
			);
		}

		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (!AssetEditorSubsystem)
		{
			return BuildErrorResponse(
				TEXT("ASSET_EDITOR_SUBSYSTEM_UNAVAILABLE"),
				TEXT("AssetEditorSubsystem is not available."),
				TEXT("Wait for the editor to finish initializing, then retry.")
			);
		}

		IAssetRegistry* AssetRegistry = GetAssetRegistry();
		if (!AssetRegistry)
		{
			return BuildErrorResponse(
				TEXT("ASSET_REGISTRY_UNAVAILABLE"),
				TEXT("Asset Registry is not available."),
				TEXT("Wait for the asset registry to finish loading, then retry.")
			);
		}

		const int32 ClampedMaxAssets = FMath::Clamp(MaxAssets, 1, 100);
		const TArray<UObject*> EditedAssets = AssetEditorSubsystem->GetAllEditedAssets();

		FAssetData ActiveAsset;
		const bool bHasActiveAsset = UAssetDiscoveryService::GetActiveAsset(ActiveAsset);
		const FString ActiveObjectPath = bHasActiveAsset ? ActiveAsset.GetObjectPathString() : FString();

		const TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
		RootObj->SetBoolField(TEXT("success"), true);
		RootObj->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());
		RootObj->SetNumberField(TEXT("open_asset_count"), EditedAssets.Num());
		RootObj->SetNumberField(TEXT("result_limit"), ClampedMaxAssets);
		RootObj->SetNumberField(TEXT("truncated_count"), FMath::Max(0, EditedAssets.Num() - ClampedMaxAssets));

		if (bHasActiveAsset)
		{
			RootObj->SetObjectField(TEXT("active_asset"), BuildAssetJson(ActiveAsset));
		}

		TArray<TSharedPtr<FJsonValue>> OpenEditorArray;
		OpenEditorArray.Reserve(FMath::Min(EditedAssets.Num(), ClampedMaxAssets));

		for (int32 Index = 0; Index < EditedAssets.Num() && Index < ClampedMaxAssets; ++Index)
		{
			UObject* EditedAsset = EditedAssets[Index];
			if (!EditedAsset)
			{
				continue;
			}

			const FAssetData AssetData = AssetRegistry->GetAssetByObjectPath(FSoftObjectPath(EditedAsset));
			if (!AssetData.IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject> AssetObj = BuildAssetJson(AssetData);
			const TArray<IAssetEditorInstance*> EditorInstances = AssetEditorSubsystem->FindEditorsForAsset(EditedAsset);
			AssetObj->SetBoolField(TEXT("is_active"), !ActiveObjectPath.IsEmpty() && AssetData.GetObjectPathString() == ActiveObjectPath);
			AssetObj->SetNumberField(TEXT("editor_instance_count"), EditorInstances.Num());

			double LastActivationTime = 0.0;
			TArray<TSharedPtr<FJsonValue>> EditorNameArray;
			EditorNameArray.Reserve(EditorInstances.Num());

			for (IAssetEditorInstance* EditorInstance : EditorInstances)
			{
				if (!EditorInstance)
				{
					continue;
				}

				EditorNameArray.Add(MakeShared<FJsonValueString>(EditorInstance->GetEditorName().ToString()));
				LastActivationTime = FMath::Max(LastActivationTime, EditorInstance->GetLastActivationTime());
			}

			AssetObj->SetArrayField(TEXT("editor_names"), EditorNameArray);
			AssetObj->SetNumberField(TEXT("last_activation_time"), LastActivationTime);
			OpenEditorArray.Add(MakeShared<FJsonValueObject>(AssetObj));
		}

		RootObj->SetArrayField(TEXT("open_assets"), OpenEditorArray);
		return RootObj;
	}

	static TSharedPtr<FJsonObject> BuildEditorContextJson(const TMap<FString, FString>& Params)
	{
		if (!GEditor)
		{
			return BuildErrorResponse(
				TEXT("EDITOR_UNAVAILABLE"),
				TEXT("Unreal Editor is not available yet."),
				TEXT("Wait for the editor to finish loading, then retry.")
			);
		}

		const int32 MaxSelectedAssets = ExtractIntParam(Params, TEXT("max_selected_assets"), 20, 1, 100);
		const int32 MaxOpenEditors = ExtractIntParam(Params, TEXT("max_open_editors"), 20, 1, 100);
		const bool bIncludeActorSample = ExtractBoolParam(Params, TEXT("include_actor_sample"), false);
		const int32 MaxActorSample = ExtractIntParam(Params, TEXT("max_actor_sample"), 20, 1, 100);

		const TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
		RootObj->SetBoolField(TEXT("success"), true);
		RootObj->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());
		RootObj->SetObjectField(TEXT("mcp"), BuildMcpSummaryJson());
		RootObj->SetObjectField(TEXT("tool_registry"), BuildToolRegistrySummaryJson());
		RootObj->SetObjectField(TEXT("project"), BuildProjectInfoJson());
		RootObj->SetObjectField(TEXT("current_level"), BuildCurrentLevelJson(bIncludeActorSample, MaxActorSample));
		RootObj->SetObjectField(TEXT("selected_assets"), BuildSelectedAssetsJson(MaxSelectedAssets));
		RootObj->SetObjectField(TEXT("open_editors"), BuildOpenEditorsJson(MaxOpenEditors));

		UWorld* World = nullptr;
		FString ErrorCode;
		FString ErrorMessage;
		FString SuggestedNextStep;
		const bool bHasEditorWorld = TryGetEditorWorld(World, ErrorCode, ErrorMessage, SuggestedNextStep);

		const FMCPServerDiagnosticsSnapshot Snapshot = FMCPServer::Get().GetDiagnosticsSnapshot(20);
		TArray<TSharedPtr<FJsonValue>> RecommendationArray;

		if (!Snapshot.bIsRunning)
		{
			RecommendationArray.Add(MakeShared<FJsonValueString>(TEXT("Enable the MCP server and confirm the configured port is free before reconnecting Codex/Cursor.")));
		}
		else if (GetActiveClientCount(Snapshot) == 0)
		{
			RecommendationArray.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Connect Codex/Cursor to %s."), *Snapshot.ServerUrl)));
		}

		if (Snapshot.TotalRpcRequests == 0)
		{
			RecommendationArray.Add(MakeShared<FJsonValueString>(TEXT("After connecting, run self_test or project_info to confirm the MCP tool chain is healthy.")));
		}

		if (!bHasEditorWorld)
		{
			RecommendationArray.Add(MakeShared<FJsonValueString>(TEXT("Wait for the current level to finish loading before requesting asset or world edits.")));
		}
		else
		{
			FString MutationReadiness;
			if (!IsWorldMutationReady(World, MutationReadiness))
			{
				RecommendationArray.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("World edits are not ready yet: %s"), *MutationReadiness)));
			}
		}

		RootObj->SetArrayField(TEXT("recommended_next_steps"), RecommendationArray);
		return RootObj;
	}

	static TSharedPtr<FJsonObject> BuildContextOverviewJson()
	{
		if (!GEditor)
		{
			return BuildErrorResponse(
				TEXT("EDITOR_UNAVAILABLE"),
				TEXT("Unreal Editor is not available yet."),
				TEXT("Wait for the editor to finish loading, then retry.")
			);
		}

		const TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
		RootObj->SetBoolField(TEXT("success"), true);
		RootObj->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());

		const FMCPServerDiagnosticsSnapshot Snapshot = FMCPServer::Get().GetDiagnosticsSnapshot(10);
		const TSharedPtr<FJsonObject> MpcObj = MakeShared<FJsonObject>();
		MpcObj->SetBoolField(TEXT("server_running"), Snapshot.bIsRunning);
		MpcObj->SetStringField(TEXT("server_url"), Snapshot.ServerUrl);
		MpcObj->SetNumberField(TEXT("active_clients"), GetActiveClientCount(Snapshot));
		MpcObj->SetNumberField(TEXT("rpc_request_count"), Snapshot.TotalRpcRequests);
		MpcObj->SetStringField(TEXT("last_tool_call"), Snapshot.LastToolsCallToolName);
		RootObj->SetObjectField(TEXT("mcp"), MpcObj);

		const TSharedPtr<FJsonObject> ProjectObj = MakeShared<FJsonObject>();
		ProjectObj->SetStringField(TEXT("project_name"), FApp::GetProjectName());
		ProjectObj->SetStringField(TEXT("project_dir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		ProjectObj->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		RootObj->SetObjectField(TEXT("project"), ProjectObj);

		UWorld* World = nullptr;
		FString ErrorCode;
		FString ErrorMessage;
		FString SuggestedNextStep;
		const bool bHasEditorWorld = TryGetEditorWorld(World, ErrorCode, ErrorMessage, SuggestedNextStep);

		const TSharedPtr<FJsonObject> LevelObj = MakeShared<FJsonObject>();
		LevelObj->SetBoolField(TEXT("available"), bHasEditorWorld);
		if (bHasEditorWorld)
		{
			FString MutationReadiness;
			const bool bMutationReady = IsWorldMutationReady(World, MutationReadiness);
			LevelObj->SetStringField(TEXT("world_name"), World->GetName());
			LevelObj->SetStringField(TEXT("map_name"), World->GetMapName());
			LevelObj->SetStringField(TEXT("package_name"), World->GetOutermost() ? World->GetOutermost()->GetName() : FString());
			LevelObj->SetBoolField(TEXT("is_dirty"), World->GetOutermost() ? World->GetOutermost()->IsDirty() : false);
			LevelObj->SetNumberField(TEXT("actor_count"), CountActors(World));
			LevelObj->SetNumberField(TEXT("selected_actor_count"), CountSelectedActors(World));
			LevelObj->SetBoolField(TEXT("world_mutation_ready"), bMutationReady);
			LevelObj->SetStringField(TEXT("world_mutation_state"), MutationReadiness);
		}
		else
		{
			LevelObj->SetStringField(TEXT("error"), ErrorMessage);
		}
		RootObj->SetObjectField(TEXT("current_level"), LevelObj);

		const TArray<FAssetData> SelectedAssets = UAssetDiscoveryService::GetContentBrowserSelections();
		const TSharedPtr<FJsonObject> SelectionObj = MakeShared<FJsonObject>();
		SelectionObj->SetNumberField(TEXT("selection_count"), SelectedAssets.Num());
		FAssetData PrimaryAsset;
		if (UAssetDiscoveryService::GetPrimaryContentBrowserSelection(PrimaryAsset))
		{
			SelectionObj->SetObjectField(TEXT("primary_selection"), BuildAssetJson(PrimaryAsset));
		}
		RootObj->SetObjectField(TEXT("selected_assets"), SelectionObj);

		const TSharedPtr<FJsonObject> EditorsObj = MakeShared<FJsonObject>();
		const TArray<FAssetData> OpenAssets = UAssetDiscoveryService::GetOpenAssets();
		EditorsObj->SetNumberField(TEXT("open_asset_count"), OpenAssets.Num());
		FAssetData ActiveAsset;
		if (UAssetDiscoveryService::GetActiveAsset(ActiveAsset))
		{
			EditorsObj->SetObjectField(TEXT("active_asset"), BuildAssetJson(ActiveAsset));
		}
		RootObj->SetObjectField(TEXT("open_editors"), EditorsObj);

		TArray<TSharedPtr<FJsonValue>> RecommendationArray;
		if (!Snapshot.bIsRunning)
		{
			RecommendationArray.Add(MakeShared<FJsonValueString>(TEXT("Enable the MCP server before requesting deeper editor automation.")));
		}
		else if (GetActiveClientCount(Snapshot) == 0)
		{
			RecommendationArray.Add(MakeShared<FJsonValueString>(TEXT("Connect an MCP client, then use editor_context or the narrow context slices if you need more detail.")));
		}

		if (!bHasEditorWorld)
		{
			RecommendationArray.Add(MakeShared<FJsonValueString>(SuggestedNextStep));
		}

		RootObj->SetArrayField(TEXT("recommended_next_steps"), RecommendationArray);
		return RootObj;
	}

	static TSharedPtr<FJsonObject> BuildSelfTestJson()
	{
		FToolRegistry& ToolRegistry = FToolRegistry::Get();
		const FMCPServerDiagnosticsSnapshot Snapshot = FMCPServer::Get().GetDiagnosticsSnapshot(20);
		TArray<TSharedPtr<FJsonValue>> CheckArray;
		int32 PassCount = 0;
		int32 WarnCount = 0;
		int32 FailCount = 0;

		auto AddCheck = [&CheckArray, &PassCount, &WarnCount, &FailCount](const FString& Name, const FString& Status, const FString& Details)
		{
			const TSharedPtr<FJsonObject> CheckObj = MakeShared<FJsonObject>();
			CheckObj->SetStringField(TEXT("name"), Name);
			CheckObj->SetStringField(TEXT("status"), Status);
			CheckObj->SetStringField(TEXT("details"), Details);
			CheckArray.Add(MakeShared<FJsonValueObject>(CheckObj));

			if (Status == TEXT("pass"))
			{
				++PassCount;
			}
			else if (Status == TEXT("warn"))
			{
				++WarnCount;
			}
			else
			{
				++FailCount;
			}
		};

		AddCheck(
			TEXT("mcp_server_running"),
			Snapshot.bIsRunning ? TEXT("pass") : TEXT("fail"),
			Snapshot.bIsRunning
				? FString::Printf(TEXT("Server is listening at %s"), *Snapshot.ServerUrl)
				: TEXT("Server is not running; Codex/Cursor cannot reach the plugin yet.")
		);

		AddCheck(
			TEXT("tool_registry_initialized"),
			ToolRegistry.IsInitialized() ? TEXT("pass") : TEXT("fail"),
			ToolRegistry.IsInitialized()
				? FString::Printf(TEXT("%d tools registered, %d enabled"), ToolRegistry.GetAllTools().Num(), ToolRegistry.GetEnabledTools().Num())
				: TEXT("Tool registry is not initialized.")
		);

		AddCheck(
			TEXT("editor_available"),
			GEditor ? TEXT("pass") : TEXT("fail"),
			GEditor ? TEXT("Editor subsystem is available.") : TEXT("GEditor is null.")
		);

		UWorld* World = nullptr;
		FString ErrorCode;
		FString ErrorMessage;
		FString SuggestedNextStep;
		const bool bHasEditorWorld = TryGetEditorWorld(World, ErrorCode, ErrorMessage, SuggestedNextStep);
		AddCheck(
			TEXT("editor_world_available"),
			bHasEditorWorld ? TEXT("pass") : TEXT("warn"),
			bHasEditorWorld ? TEXT("Editor world is loaded.") : ErrorMessage
		);

		if (bHasEditorWorld)
		{
			FString MutationReadiness;
			const bool bMutationReady = IsWorldMutationReady(World, MutationReadiness);
			AddCheck(
				TEXT("world_mutation_ready"),
				bMutationReady ? TEXT("pass") : TEXT("warn"),
				MutationReadiness
			);
		}

		const bool bPythonDiscoveryReady = UPythonTools::GetDiscoveryService().IsValid();
		AddCheck(
			TEXT("python_discovery_ready"),
			bPythonDiscoveryReady ? TEXT("pass") : TEXT("fail"),
			bPythonDiscoveryReady ? TEXT("Python discovery service initialized.") : TEXT("Python discovery service is unavailable.")
		);

		const TArray<FString> RequiredToolNames = GetContextToolNames();
		bool bAllRegistered = true;
		bool bAllEnabled = true;
		for (const FString& ToolName : RequiredToolNames)
		{
			bAllRegistered &= (ToolRegistry.FindTool(ToolName) != nullptr);
			bAllEnabled &= ToolRegistry.IsToolEnabled(ToolName);
		}

		AddCheck(
			TEXT("context_tools_registered"),
			bAllRegistered ? TEXT("pass") : TEXT("fail"),
			bAllRegistered ? TEXT("All context tools are registered.") : TEXT("One or more context tools are missing from the registry.")
		);

		AddCheck(
			TEXT("context_tools_enabled"),
			bAllEnabled ? TEXT("pass") : TEXT("warn"),
			bAllEnabled ? TEXT("All context tools are enabled.") : TEXT("One or more context tools are disabled in Tool Manager.")
		);

		AddCheck(
			TEXT("mcp_client_connected"),
			GetActiveClientCount(Snapshot) > 0 ? TEXT("pass") : TEXT("warn"),
			GetActiveClientCount(Snapshot) > 0
				? FString::Printf(TEXT("%d client(s) currently connected."), GetActiveClientCount(Snapshot))
				: TEXT("No active MCP client connection detected.")
		);

		AddCheck(
			TEXT("recent_rpc_activity"),
			Snapshot.TotalRpcRequests > 0 ? TEXT("pass") : TEXT("warn"),
			Snapshot.TotalRpcRequests > 0
				? FString::Printf(TEXT("%lld RPC request(s) processed."), Snapshot.TotalRpcRequests)
				: TEXT("No RPC traffic has reached the MCP server yet.")
		);

		const TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
		RootObj->SetBoolField(TEXT("success"), FailCount == 0);
		RootObj->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());
		RootObj->SetArrayField(TEXT("checks"), CheckArray);

		const TSharedPtr<FJsonObject> SummaryObj = MakeShared<FJsonObject>();
		SummaryObj->SetNumberField(TEXT("pass_count"), PassCount);
		SummaryObj->SetNumberField(TEXT("warn_count"), WarnCount);
		SummaryObj->SetNumberField(TEXT("fail_count"), FailCount);
		RootObj->SetObjectField(TEXT("summary"), SummaryObj);
		RootObj->SetObjectField(TEXT("mcp"), BuildMcpSummaryJson());
		RootObj->SetObjectField(TEXT("tool_registry"), BuildToolRegistrySummaryJson());

		TArray<TSharedPtr<FJsonValue>> RecommendationArray;
		if (!Snapshot.bIsRunning)
		{
			RecommendationArray.Add(MakeShared<FJsonValueString>(TEXT("Start the MCP server from the dashboard settings before reconnecting Codex/Cursor.")));
		}
		else if (GetActiveClientCount(Snapshot) == 0)
		{
			RecommendationArray.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Point Codex/Cursor at %s and reconnect."), *Snapshot.ServerUrl)));
		}

		if (Snapshot.TotalRpcRequests == 0)
		{
			RecommendationArray.Add(MakeShared<FJsonValueString>(TEXT("After connecting, run project_info or editor_context to confirm end-to-end MCP connectivity.")));
		}

		if (!bHasEditorWorld)
		{
			RecommendationArray.Add(MakeShared<FJsonValueString>(SuggestedNextStep));
		}

		if (!bAllEnabled)
		{
			RecommendationArray.Add(MakeShared<FJsonValueString>(TEXT("Re-enable the disabled context tools from the dashboard Tools panel.")));
		}

		RootObj->SetArrayField(TEXT("recommended_next_steps"), RecommendationArray);
		return RootObj;
	}
}

REGISTER_VIBEUE_TOOL(editor_context,
	"Read-only snapshot of the current Unreal editor state: MCP server status, project info, current level summary, selected assets, and open asset editors. Use this before large tool-driven edits to ground Codex/Cursor in the live editor context.",
	"Context",
	TOOL_PARAMS(
		TOOL_PARAM_DEFAULT("max_selected_assets", "Maximum selected assets to include (default 20)", "number", "20"),
		TOOL_PARAM_DEFAULT("max_open_editors", "Maximum open asset editors to include (default 20)", "number", "20"),
		TOOL_PARAM_DEFAULT("include_actor_sample", "Whether to include a sample of actors from the current level (default false)", "boolean", "false"),
		TOOL_PARAM_DEFAULT("max_actor_sample", "Maximum actors to include when include_actor_sample=true (default 20)", "number", "20")
	),
	{
		return SerializeJsonObject(BuildEditorContextJson(Params));
	}
);

REGISTER_VIBEUE_TOOL(context_overview,
	"Lightweight read-only overview of the live editor context. Returns a compact MCP, project, current level, selection, and open-editor summary so callers can decide whether they need deeper context tools like editor_context, current_level, or selected_assets.",
	"Context",
	TOOL_PARAMS(),
	{
		return SerializeJsonObject(BuildContextOverviewJson());
	}
);

REGISTER_VIBEUE_TOOL(project_info,
	"Read-only project and engine metadata: project file path, directories, engine version, VibeUE plugin paths, and default map settings.",
	"Context",
	TOOL_PARAMS(),
	{
		return SerializeJsonObject(BuildProjectInfoJson());
	}
);

REGISTER_VIBEUE_TOOL(current_level,
	"Read-only summary of the currently open level: map/package identity, dirty state, actor counts, streaming status, and world-mutation readiness.",
	"Context",
	TOOL_PARAMS(
		TOOL_PARAM_DEFAULT("include_actor_sample", "Whether to include a sample of actors from the current level (default false)", "boolean", "false"),
		TOOL_PARAM_DEFAULT("max_actor_sample", "Maximum actors to include when include_actor_sample=true (default 20)", "number", "20")
	),
	{
		const bool bIncludeActorSample = ExtractBoolParam(Params, TEXT("include_actor_sample"), false);
		const int32 MaxActorSample = ExtractIntParam(Params, TEXT("max_actor_sample"), 20, 1, 100);
		return SerializeJsonObject(BuildCurrentLevelJson(bIncludeActorSample, MaxActorSample));
	}
);

REGISTER_VIBEUE_TOOL(selected_assets,
	"Read-only snapshot of the current Content Browser selection. Returns the primary selected asset plus a bounded list of selected assets with package/class metadata.",
	"Context",
	TOOL_PARAMS(
		TOOL_PARAM_DEFAULT("max_assets", "Maximum selected assets to include (default 50)", "number", "50")
	),
	{
		const int32 MaxAssets = ExtractIntParam(Params, TEXT("max_assets"), 50, 1, 100);
		return SerializeJsonObject(BuildSelectedAssetsJson(MaxAssets));
	}
);

REGISTER_VIBEUE_TOOL(open_editors,
	"Read-only list of assets currently open in editor tabs. Includes the active asset, editor instance names, and basic asset metadata.",
	"Context",
	TOOL_PARAMS(
		TOOL_PARAM_DEFAULT("max_assets", "Maximum open assets to include (default 50)", "number", "50")
	),
	{
		const int32 MaxAssets = ExtractIntParam(Params, TEXT("max_assets"), 50, 1, 100);
		return SerializeJsonObject(BuildOpenEditorsJson(MaxAssets));
	}
);

REGISTER_VIBEUE_TOOL(self_test,
	"Read-only MCP and editor health check. Verifies server status, tool registration, editor/world readiness, Python discovery availability, and whether a client has connected.",
	"Context",
	TOOL_PARAMS(),
	{
		return SerializeJsonObject(BuildSelfTestJson());
	}
);
