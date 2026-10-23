// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Core/ToolRegistry.h"
#include "Tools/ToolEnvelopeUtils.h"

#include "Core/ErrorCodes.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Json.h"
#include "LevelEditorSubsystem.h"
#include "Subsystems/UnrealEditorSubsystem.h"

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

	static FString SerializeJsonObject(const TSharedPtr<FJsonObject>& JsonObject)
	{
		return FToolEnvelopeUtils::SerializeJsonObject(JsonObject);
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

	static int32 ExtractIntParam(const TMap<FString, FString>& Params, const FString& FieldName, int32 DefaultValue, int32 MinValue, int32 MaxValue)
	{
		const FString Value = ExtractParamFromJson(Params, FieldName);
		if (Value.IsEmpty())
		{
			return FMath::Clamp(DefaultValue, MinValue, MaxValue);
		}

		return FMath::Clamp(FCString::Atoi(*Value), MinValue, MaxValue);
	}

	static TSharedPtr<FJsonObject> BuildErrorResponse(const FString& ErrorCode, const FString& ErrorMessage)
	{
		const TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
		ErrorObj->SetBoolField(TEXT("success"), false);
		ErrorObj->SetStringField(TEXT("error_code"), ErrorCode);
		ErrorObj->SetStringField(TEXT("error_message"), ErrorMessage);
		return ErrorObj;
	}

	static FString WorldTypeToString(const EWorldType::Type WorldType)
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

	static TSharedPtr<FJsonObject> BuildWorldSummary(UWorld* World)
	{
		if (!World)
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject> WorldObj = MakeShared<FJsonObject>();
		WorldObj->SetStringField(TEXT("name"), World->GetName());
		WorldObj->SetStringField(TEXT("path"), World->GetPathName());
		WorldObj->SetStringField(TEXT("package"), World->GetOutermost() ? World->GetOutermost()->GetName() : FString());
		WorldObj->SetStringField(TEXT("world_type"), WorldTypeToString(World->WorldType));
		WorldObj->SetBoolField(TEXT("is_game_world"), World->IsGameWorld());
		WorldObj->SetBoolField(TEXT("is_play_in_editor"), World->IsPlayInEditor());
		return WorldObj;
	}

	static bool PassesActorFilter(AActor* Actor, const FString& ClassFilter, const FString& LabelFilter, const bool bIncludeHidden)
	{
		if (!Actor)
		{
			return false;
		}

		if (!bIncludeHidden && Actor->IsHidden())
		{
			return false;
		}

		if (!LabelFilter.IsEmpty())
		{
			const FString LowerLabelFilter = LabelFilter.ToLower();
			if (!Actor->GetActorLabel().ToLower().Contains(LowerLabelFilter) &&
				!Actor->GetName().ToLower().Contains(LowerLabelFilter))
			{
				return false;
			}
		}

		if (!ClassFilter.IsEmpty())
		{
			const FString LowerClassFilter = ClassFilter.ToLower();
			if (!Actor->GetClass()->GetName().ToLower().Contains(LowerClassFilter) &&
				!Actor->GetClass()->GetPathName().ToLower().Contains(LowerClassFilter))
			{
				return false;
			}
		}

		return true;
	}

	static TSharedPtr<FJsonObject> BuildActorSummary(AActor* Actor)
	{
		const TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
		ActorObj->SetStringField(TEXT("actor_name"), Actor ? Actor->GetName() : FString());
		ActorObj->SetStringField(TEXT("actor_label"), Actor ? Actor->GetActorLabel() : FString());
		ActorObj->SetStringField(TEXT("actor_class"), Actor ? Actor->GetClass()->GetPathName() : FString());
		ActorObj->SetStringField(TEXT("actor_path"), Actor ? Actor->GetPathName() : FString());
		ActorObj->SetStringField(TEXT("folder_path"), Actor ? Actor->GetFolderPath().ToString() : FString());
		return ActorObj;
	}

	static FString HandlePieSessionStatus()
	{
		if (!GEditor)
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::OPERATION_NOT_ALLOWED, TEXT("Editor is not available.")));
		}

		ULevelEditorSubsystem* LevelEditorSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
		UUnrealEditorSubsystem* UnrealEditorSubsystem = GEditor->GetEditorSubsystem<UUnrealEditorSubsystem>();
		if (!LevelEditorSubsystem || !UnrealEditorSubsystem)
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::OPERATION_NOT_ALLOWED, TEXT("Required editor subsystems are not available.")));
		}

		UWorld* EditorWorld = UnrealEditorSubsystem->GetEditorWorld();
		UWorld* GameWorld = UnrealEditorSubsystem->GetGameWorld();

		const TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
		ResponseObj->SetBoolField(TEXT("success"), true);
		ResponseObj->SetBoolField(TEXT("in_pie"), LevelEditorSubsystem->IsInPlayInEditor());
		ResponseObj->SetBoolField(TEXT("has_editor_world"), EditorWorld != nullptr);
		ResponseObj->SetBoolField(TEXT("has_game_world"), GameWorld != nullptr);
		ResponseObj->SetStringField(
			TEXT("next_step_hint"),
			TEXT("After pie_session_control start, poll pie_session_status until in_pie=true and has_game_world=true before inspecting runtime actors.")
		);

		if (const TSharedPtr<FJsonObject> EditorWorldObj = BuildWorldSummary(EditorWorld))
		{
			ResponseObj->SetObjectField(TEXT("editor_world"), EditorWorldObj);
		}

		if (const TSharedPtr<FJsonObject> GameWorldObj = BuildWorldSummary(GameWorld))
		{
			ResponseObj->SetObjectField(TEXT("game_world"), GameWorldObj);
		}

		return SerializeJsonObject(ResponseObj);
	}

	static FString HandlePieSessionControl(const TMap<FString, FString>& Params)
	{
		if (!GEditor)
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::OPERATION_NOT_ALLOWED, TEXT("Editor is not available.")));
		}

		ULevelEditorSubsystem* LevelEditorSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
		if (!LevelEditorSubsystem)
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::OPERATION_NOT_ALLOWED, TEXT("LevelEditorSubsystem is not available.")));
		}

		const FString Action = ExtractParamFromJson(Params, TEXT("action")).TrimStartAndEnd().ToLower();
		if (Action.IsEmpty())
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_MISSING, TEXT("Parameter 'action' is required.")));
		}

		const bool bInPie = LevelEditorSubsystem->IsInPlayInEditor();
		const TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
		ResponseObj->SetBoolField(TEXT("success"), true);
		ResponseObj->SetStringField(TEXT("action"), Action);
		ResponseObj->SetBoolField(TEXT("in_pie_before"), bInPie);

		if (Action == TEXT("start"))
		{
			if (bInPie)
			{
				ResponseObj->SetStringField(TEXT("state"), TEXT("already_running"));
				ResponseObj->SetBoolField(TEXT("requested"), false);
			}
			else
			{
				LevelEditorSubsystem->EditorRequestBeginPlay();
				ResponseObj->SetStringField(TEXT("state"), TEXT("start_requested"));
				ResponseObj->SetBoolField(TEXT("requested"), true);
			}
		}
		else if (Action == TEXT("stop"))
		{
			if (!bInPie)
			{
				ResponseObj->SetStringField(TEXT("state"), TEXT("already_stopped"));
				ResponseObj->SetBoolField(TEXT("requested"), false);
			}
			else
			{
				LevelEditorSubsystem->EditorRequestEndPlay();
				ResponseObj->SetStringField(TEXT("state"), TEXT("stop_requested"));
				ResponseObj->SetBoolField(TEXT("requested"), true);
			}
		}
		else
		{
			return SerializeJsonObject(
				BuildErrorResponse(
					VibeUE::ErrorCodes::PARAM_INVALID,
					TEXT("Parameter 'action' must be one of: start, stop.")
				)
			);
		}

		ResponseObj->SetBoolField(TEXT("in_pie_after_request"), LevelEditorSubsystem->IsInPlayInEditor());
		return SerializeJsonObject(ResponseObj);
	}

	static FString HandleActiveWorldActors(const TMap<FString, FString>& Params)
	{
		if (!GEditor)
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::OPERATION_NOT_ALLOWED, TEXT("Editor is not available.")));
		}

		UUnrealEditorSubsystem* UnrealEditorSubsystem = GEditor->GetEditorSubsystem<UUnrealEditorSubsystem>();
		if (!UnrealEditorSubsystem)
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::OPERATION_NOT_ALLOWED, TEXT("UnrealEditorSubsystem is not available.")));
		}

		const FString WorldSelector = ExtractParamFromJson(Params, TEXT("world")).TrimStartAndEnd().ToLower();
		const FString LabelFilter = ExtractParamFromJson(Params, TEXT("label_filter"));
		const FString ClassFilter = ExtractParamFromJson(Params, TEXT("class_filter"));
		const bool bIncludeHidden = ExtractBoolParam(Params, TEXT("include_hidden"), false);
		const int32 MaxResults = ExtractIntParam(Params, TEXT("max_results"), 200, 0, 10000);

		UWorld* TargetWorld = nullptr;
		FString ResolvedWorld = WorldSelector;
		if (WorldSelector.IsEmpty() || WorldSelector == TEXT("game"))
		{
			ResolvedWorld = TEXT("game");
			TargetWorld = UnrealEditorSubsystem->GetGameWorld();
		}
		else if (WorldSelector == TEXT("editor"))
		{
			ResolvedWorld = TEXT("editor");
			TargetWorld = UnrealEditorSubsystem->GetEditorWorld();
		}
		else
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, TEXT("Parameter 'world' must be one of: game, editor.")));
		}

		if (!TargetWorld)
		{
			return SerializeJsonObject(
				BuildErrorResponse(
					VibeUE::ErrorCodes::PROPERTY_NOT_FOUND,
					FString::Printf(TEXT("Requested %s world is not available."), *ResolvedWorld)
				)
			);
		}

		TArray<TSharedPtr<FJsonValue>> ActorsArray;
		int32 TotalMatched = 0;
		for (TActorIterator<AActor> It(TargetWorld); It; ++It)
		{
			AActor* Actor = *It;
			if (!PassesActorFilter(Actor, ClassFilter, LabelFilter, bIncludeHidden))
			{
				continue;
			}

			++TotalMatched;
			if (MaxResults == 0 || ActorsArray.Num() < MaxResults)
			{
				ActorsArray.Add(MakeShared<FJsonValueObject>(BuildActorSummary(Actor)));
			}
		}

		const TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
		ResponseObj->SetBoolField(TEXT("success"), true);
		ResponseObj->SetStringField(TEXT("world"), ResolvedWorld);
		ResponseObj->SetBoolField(TEXT("include_hidden"), bIncludeHidden);
		ResponseObj->SetStringField(TEXT("label_filter"), LabelFilter);
		ResponseObj->SetStringField(TEXT("class_filter"), ClassFilter);
		ResponseObj->SetNumberField(TEXT("matched_count"), TotalMatched);
		ResponseObj->SetNumberField(TEXT("returned_count"), ActorsArray.Num());
		ResponseObj->SetObjectField(TEXT("world_summary"), BuildWorldSummary(TargetWorld));
		ResponseObj->SetArrayField(TEXT("actors"), ActorsArray);
		return SerializeJsonObject(ResponseObj);
	}
}

REGISTER_VIBEUE_TOOL(pie_session_status,
	"Read current PIE session status. Use this after pie_session_control to confirm when the PIE world actually exists before any runtime inspection.",
	"Editor",
	TOOL_PARAMS(),
	{
		return HandlePieSessionStatus();
	}
);

REGISTER_VIBEUE_TOOL(pie_session_control,
	"Start or stop PIE using native editor APIs. IMPORTANT: This only requests the lifecycle change. Poll pie_session_status until the game world is actually available or gone.",
	"Editor",
	TOOL_PARAMS(
		TOOL_PARAM("action", "PIE session action: start or stop.", "string", true)
	),
	{
		return HandlePieSessionControl(Params);
	}
);

REGISTER_VIBEUE_TOOL(active_world_actors,
	"List actors from the active editor world or PIE game world without using execute_python_code. Use this after pie_session_status reports has_game_world=true.",
	"Editor",
	TOOL_PARAMS(
		TOOL_PARAM("world", "Target world: game or editor. Defaults to game.", "string", false),
		TOOL_PARAM("label_filter", "Optional substring filter applied to actor label and name.", "string", false),
		TOOL_PARAM("class_filter", "Optional substring filter applied to actor class name and path.", "string", false),
		TOOL_PARAM("include_hidden", "Whether to include hidden actors. Default false.", "boolean", false),
		TOOL_PARAM("max_results", "Maximum actors to return. Default 200, 0 means unlimited.", "number", false)
	),
	{
		return HandleActiveWorldActors(Params);
	}
);

void ForceLinkPIESessionTools()
{
}
