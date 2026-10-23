#include "Core/ToolRegistry.h"
#include "PythonAPI/UAssetDiscoveryService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/WidgetTree.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "Json.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "WidgetBlueprint.h"
#include "Animation/WidgetAnimation.h"

namespace
{
	static const FTopLevelAssetPath WidgetBlueprintClassPath(TEXT("/Script/UMGEditor.WidgetBlueprint"));
	static const FTopLevelAssetPath EditorUtilityWidgetBlueprintClassPath(TEXT("/Script/Blutility.EditorUtilityWidgetBlueprint"));

	struct FWidgetLogicAggregate
	{
		int32 TotalNodes = 0;
		int32 EventGraphCount = 0;
		int32 FunctionGraphCount = 0;
		int32 MacroGraphCount = 0;
		int32 EntryPointCount = 0;
		int32 FunctionCallCount = 0;
		int32 LatentCallCount = 0;
		int32 TimerCallCount = 0;
		int32 AnimationCallCount = 0;
		int32 VisibilityCallCount = 0;
		int32 UiStateCallCount = 0;
		int32 WidgetTreeCallCount = 0;
		int32 InputCallCount = 0;
		bool bUsesTick = false;
		bool bUsesLifecycleEvents = false;
		bool bUsesWidgetAnimations = false;
		bool bUsesTiming = false;
		bool bUsesWidgetTreeMutation = false;
		bool bUsesVisibilityMutation = false;
		bool bUsesUiStateMutation = false;
		bool bUsesInputEvents = false;
		bool bUsesControlFlow = false;
		TMap<FString, int32> NodeClassCounts;
		TMap<FString, int32> CallCategoryCounts;
		TMap<FString, int32> FunctionCallCounts;
	};

	static FString ExtractParam(const TMap<FString, FString>& Params, const FString& FieldName)
	{
		if (const FString* DirectValue = Params.Find(FieldName))
		{
			return *DirectValue;
		}

		FString CapitalizedField = FieldName;
		if (!CapitalizedField.IsEmpty())
		{
			CapitalizedField[0] = FChar::ToUpper(CapitalizedField[0]);
		}

		if (const FString* CapitalizedValue = Params.Find(CapitalizedField))
		{
			return *CapitalizedValue;
		}

		const FString* ParamsJsonString = Params.Find(TEXT("ParamsJson"));
		if (!ParamsJsonString)
		{
			return FString();
		}

		TSharedPtr<FJsonObject> JsonObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ParamsJsonString);
		if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
		{
			return FString();
		}

		FString Value;
		if (JsonObject->TryGetStringField(FieldName, Value))
		{
			return Value;
		}

		return FString();
	}

	static bool ExtractBoolParam(const TMap<FString, FString>& Params, const FString& FieldName, bool bDefaultValue)
	{
		const FString Value = ExtractParam(Params, FieldName);
		if (Value.IsEmpty())
		{
			return bDefaultValue;
		}

		return Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("1"));
	}

	static int32 ExtractIntParam(const TMap<FString, FString>& Params, const FString& FieldName, int32 DefaultValue, int32 MinValue, int32 MaxValue)
	{
		const FString Value = ExtractParam(Params, FieldName);
		if (Value.IsEmpty())
		{
			return DefaultValue;
		}

		return FMath::Clamp(FCString::Atoi(*Value), MinValue, MaxValue);
	}

	static FString SerializeJson(const TSharedPtr<FJsonObject>& JsonObject, bool bPretty = false)
	{
		FString Result;
		if (bPretty)
		{
			const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Result);
			FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
		}
		else
		{
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
			FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
		}
		return Result;
	}

	static void AddStringArrayField(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		JsonObject->SetArrayField(FieldName, JsonValues);
	}

	static void AddJsonObjectArrayField(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, const TArray<TSharedPtr<FJsonObject>>& Objects)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Objects.Num());
		for (const TSharedPtr<FJsonObject>& Object : Objects)
		{
			JsonValues.Add(MakeShared<FJsonValueObject>(Object));
		}
		JsonObject->SetArrayField(FieldName, JsonValues);
	}

	static FString GetJsonStringField(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, const FString& DefaultValue = TEXT(""))
	{
		if (!JsonObject.IsValid())
		{
			return DefaultValue;
		}

		FString Value;
		return JsonObject->TryGetStringField(FieldName, Value) ? Value : DefaultValue;
	}

	static TSharedPtr<FJsonObject> BuildErrorResponse(
		const FString& Action,
		const FString& ErrorCode,
		const FString& ErrorMessage,
		const TArray<FString>& VerificationHints = {},
		const TArray<FString>& Warnings = {})
	{
		const TSharedPtr<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
		ErrorObject->SetBoolField(TEXT("success"), false);
		ErrorObject->SetStringField(TEXT("action"), Action);
		ErrorObject->SetStringField(TEXT("error_code"), ErrorCode);
		ErrorObject->SetStringField(TEXT("error"), ErrorMessage);
		ErrorObject->SetStringField(TEXT("error_message"), ErrorMessage);
		AddStringArrayField(ErrorObject, TEXT("verification_hints"), VerificationHints);
		AddStringArrayField(ErrorObject, TEXT("warnings"), Warnings);
		return ErrorObject;
	}

	static TSharedPtr<FJsonObject> BuildSuccessResponse(
		const FString& Action,
		const FString& Summary,
		TSharedPtr<FJsonObject> Data,
		const TArray<FString>& ChangedItems = {},
		const TArray<FString>& VerificationHints = {},
		const TArray<FString>& Warnings = {},
		const TArray<FString>& AffectedAssets = {})
	{
		const TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
		ResultObject->SetBoolField(TEXT("success"), true);
		ResultObject->SetStringField(TEXT("action"), Action);
		ResultObject->SetStringField(TEXT("summary"), Summary);
		if (Data.IsValid())
		{
			ResultObject->SetObjectField(TEXT("data"), Data);
		}
		AddStringArrayField(ResultObject, TEXT("changed_items"), ChangedItems);
		AddStringArrayField(ResultObject, TEXT("verification_hints"), VerificationHints);
		AddStringArrayField(ResultObject, TEXT("warnings"), Warnings);
		AddStringArrayField(ResultObject, TEXT("affected_assets"), AffectedAssets);
		return ResultObject;
	}

	static FString GetPackagePathFromAnyWidgetPath(const FString& WidgetPath)
	{
		return WidgetPath.Contains(TEXT("."))
			? FPackageName::ObjectPathToPackageName(WidgetPath)
			: WidgetPath;
	}

	static FString GetWidgetLogicDirectory()
	{
		FString SavedDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
		FPaths::CollapseRelativeDirectories(SavedDir);
		FPaths::NormalizeFilename(SavedDir);
		return FPaths::Combine(SavedDir, TEXT("VibeUE"), TEXT("WidgetLogic"));
	}

	static FString MakeWidgetLogicBaseName(const FString& WidgetPath)
	{
		FString NormalizedPath = GetPackagePathFromAnyWidgetPath(WidgetPath);
		NormalizedPath.ReplaceInline(TEXT("/"), TEXT("__"));
		NormalizedPath = FPaths::MakeValidFileName(NormalizedPath);
		if (NormalizedPath.IsEmpty())
		{
			NormalizedPath = TEXT("WidgetLogic");
		}
		return NormalizedPath;
	}

	static FString BuildDefaultLogicPath(const FString& WidgetPath)
	{
		return FPaths::Combine(GetWidgetLogicDirectory(), MakeWidgetLogicBaseName(WidgetPath) + TEXT(".logic.json"));
	}

	static FString BuildDefaultLogicReviewPath(const FString& WidgetPath)
	{
		return FPaths::Combine(GetWidgetLogicDirectory(), MakeWidgetLogicBaseName(WidgetPath) + TEXT(".logic.review.md"));
	}

	static FString BuildDefaultLogicManifestPath(const FString& PathFilter)
	{
		FString BaseName = PathFilter;
		BaseName.ReplaceInline(TEXT("/"), TEXT("__"));
		BaseName = FPaths::MakeValidFileName(BaseName);
		if (BaseName.IsEmpty())
		{
			BaseName = TEXT("AllWidgetBlueprintLogic");
		}
		return FPaths::Combine(GetWidgetLogicDirectory(), BaseName + TEXT(".logic.batch.json"));
	}

	static FString ResolveOutputPath(const FString& RequestedPath, const FString& DefaultPath)
	{
		const FString Candidate = RequestedPath.IsEmpty() ? DefaultPath : RequestedPath;
		if (Candidate.IsEmpty())
		{
			return FString();
		}

		FString ResolvedPath = FPaths::IsRelative(Candidate)
			? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Candidate)
			: Candidate;
		FPaths::CollapseRelativeDirectories(ResolvedPath);
		FPaths::NormalizeFilename(ResolvedPath);
		return ResolvedPath;
	}

	static bool SaveTextFile(const FString& OutputPath, const FString& Content)
	{
		const FString Directory = FPaths::GetPath(OutputPath);
		IFileManager::Get().MakeDirectory(*Directory, true);
		return FFileHelper::SaveStringToFile(
			Content,
			*OutputPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(),
			FILEWRITE_None
		);
	}

	static FString SanitizeInlineMarkdownText(const FString& Value)
	{
		FString Sanitized = Value;
		Sanitized.ReplaceInline(TEXT("\r"), TEXT(" "));
		Sanitized.ReplaceInline(TEXT("\n"), TEXT(" "));
		Sanitized.ReplaceInline(TEXT("\t"), TEXT(" "));
		while (Sanitized.ReplaceInline(TEXT("  "), TEXT(" ")) > 0)
		{
		}
		Sanitized.TrimStartAndEndInline();
		return Sanitized;
	}

	static bool ResolveWidgetAssetData(const FString& WidgetPath, FAssetData& OutAssetData)
	{
		if (WidgetPath.IsEmpty())
		{
			return false;
		}

		if (!UAssetDiscoveryService::FindAssetByPath(WidgetPath, OutAssetData))
		{
			return false;
		}

		return OutAssetData.AssetClassPath == WidgetBlueprintClassPath
			|| OutAssetData.AssetClassPath == EditorUtilityWidgetBlueprintClassPath;
	}

	static bool LoadWidgetBlueprintAsset(
		const FString& WidgetPath,
		UWidgetBlueprint*& OutWidgetBlueprint,
		FString& OutCanonicalPath,
		TSharedPtr<FJsonObject>& OutError,
		const FString& Action)
	{
		FAssetData AssetData;
		if (!ResolveWidgetAssetData(WidgetPath, AssetData))
		{
			OutError = BuildErrorResponse(
				Action,
				TEXT("WIDGET_BLUEPRINT_NOT_FOUND"),
				FString::Printf(TEXT("Widget Blueprint or Editor Utility Widget '%s' was not found."), *WidgetPath),
				{ TEXT("Use widget_blueprint action=list_assets to inspect available widget assets under the requested path.") }
			);
			return false;
		}

		OutCanonicalPath = AssetData.GetObjectPathString();
		OutWidgetBlueprint = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(OutCanonicalPath));
		if (!OutWidgetBlueprint)
		{
			OutError = BuildErrorResponse(
				Action,
				TEXT("WIDGET_BLUEPRINT_LOAD_FAILED"),
				FString::Printf(TEXT("Failed to load Widget Blueprint asset '%s'."), *OutCanonicalPath),
				{ TEXT("Open the asset in the editor and confirm it is valid.") }
			);
			return false;
		}

		return true;
	}

	static TSharedPtr<FJsonObject> MakeCountEntry(const FString& Name, int32 Count)
	{
		const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Name);
		Entry->SetNumberField(TEXT("count"), Count);
		return Entry;
	}

	static TArray<TSharedPtr<FJsonObject>> BuildSortedCountObjects(const TMap<FString, int32>& Counts, int32 MaxResults = 0)
	{
		TArray<TPair<FString, int32>> Pairs;
		for (const TPair<FString, int32>& Pair : Counts)
		{
			Pairs.Add(Pair);
		}

		Pairs.Sort([](const TPair<FString, int32>& Left, const TPair<FString, int32>& Right)
		{
			if (Left.Value == Right.Value)
			{
				return Left.Key < Right.Key;
			}
			return Left.Value > Right.Value;
		});

		TArray<TSharedPtr<FJsonObject>> Objects;
		const int32 Limit = MaxResults > 0 ? FMath::Min(MaxResults, Pairs.Num()) : Pairs.Num();
		Objects.Reserve(Limit);
		for (int32 Index = 0; Index < Limit; ++Index)
		{
			Objects.Add(MakeCountEntry(Pairs[Index].Key, Pairs[Index].Value));
		}
		return Objects;
	}

	static FString DescribeGraphScope(UBlueprint* Blueprint, UEdGraph* Graph)
	{
		if (!Blueprint || !Graph)
		{
			return TEXT("unknown");
		}

		if (Blueprint->UbergraphPages.Contains(Graph))
		{
			return TEXT("event");
		}
		if (Blueprint->FunctionGraphs.Contains(Graph))
		{
			return TEXT("function");
		}
		if (Blueprint->MacroGraphs.Contains(Graph))
		{
			return TEXT("macro");
		}
		return TEXT("unknown");
	}

	static FString DescribeEntryType(UEdGraphNode* Node, const FString& EventName)
	{
		if (Node->IsA<UK2Node_CustomEvent>())
		{
			return TEXT("custom_event");
		}
		if (Node->IsA<UK2Node_ComponentBoundEvent>())
		{
			return TEXT("widget_event");
		}
		if (EventName.Equals(TEXT("Tick"), ESearchCase::IgnoreCase) || EventName.Equals(TEXT("ReceiveTick"), ESearchCase::IgnoreCase))
		{
			return TEXT("tick");
		}
		if (EventName.Contains(TEXT("Construct")) || EventName.Contains(TEXT("Destruct")) || EventName.Contains(TEXT("Initialized")) || EventName.Contains(TEXT("PreConstruct")))
		{
			return TEXT("lifecycle");
		}
		return TEXT("event");
	}

	static bool IsControlFlowNode(const FString& NodeClassName)
	{
		return NodeClassName.Contains(TEXT("ExecutionSequence"))
			|| NodeClassName.Contains(TEXT("IfThenElse"))
			|| NodeClassName.Contains(TEXT("Select"))
			|| NodeClassName.Contains(TEXT("Switch"));
	}

	static FString GetAnimationReferenceFromCallNode(UK2Node_CallFunction* CallNode)
	{
		if (!CallNode)
		{
			return FString();
		}

		for (UEdGraphPin* Pin : CallNode->Pins)
		{
			if (!Pin)
			{
				continue;
			}

			const FString PinName = Pin->PinName.ToString();
			if (!PinName.Contains(TEXT("Animation")))
			{
				continue;
			}

			if (Pin->DefaultObject)
			{
				return Pin->DefaultObject->GetPathName();
			}

			if (!Pin->DefaultValue.IsEmpty())
			{
				return Pin->DefaultValue;
			}

			if (Pin->LinkedTo.Num() > 0 && Pin->LinkedTo[0] && Pin->LinkedTo[0]->GetOwningNode())
			{
				return FString::Printf(TEXT("linked:%s"), *Pin->LinkedTo[0]->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString());
			}
		}

		return FString();
	}

	static bool IsAnimationCall(const FString& FunctionName)
	{
		return FunctionName.Equals(TEXT("PlayAnimation"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("PlayAnimationForward"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("PlayAnimationReverse"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("StopAnimation"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("PauseAnimation"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("BindToAnimationFinished"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("BindToAnimationStarted"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("StopAnimationsAndLatentActions"), ESearchCase::IgnoreCase);
	}

	static bool IsTimerCall(const FString& FunctionName)
	{
		return FunctionName.Contains(TEXT("SetTimer"))
			|| FunctionName.Contains(TEXT("ClearTimer"))
			|| FunctionName.Contains(TEXT("PauseTimer"))
			|| FunctionName.Contains(TEXT("UnPauseTimer"))
			|| FunctionName.Contains(TEXT("TimerExists"));
	}

	static bool IsTimingCall(const FString& FunctionName, const bool bIsLatent)
	{
		return bIsLatent
			|| FunctionName.Equals(TEXT("Delay"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("RetriggerableDelay"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("Gate"), ESearchCase::IgnoreCase)
			|| IsTimerCall(FunctionName);
	}

	static bool IsVisibilityCall(const FString& FunctionName)
	{
		return FunctionName.Equals(TEXT("SetVisibility"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("SetRenderOpacity"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("SetIsEnabled"), ESearchCase::IgnoreCase);
	}

	static bool IsUiStateCall(const FString& FunctionName)
	{
		return FunctionName.Equals(TEXT("SetText"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("SetBrush"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("SetBrushFromTexture"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("SetPercent"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("SetColorAndOpacity"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("SetProgressBarPercent"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("SetToolTipText"), ESearchCase::IgnoreCase);
	}

	static bool IsWidgetTreeCall(const FString& FunctionName)
	{
		return FunctionName.Equals(TEXT("CreateWidget"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("AddChild"), ESearchCase::IgnoreCase)
			|| FunctionName.Contains(TEXT("AddChildTo"))
			|| FunctionName.Equals(TEXT("RemoveChild"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("RemoveFromParent"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("ClearChildren"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("SetContent"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("InsertChildAt"), ESearchCase::IgnoreCase)
			|| FunctionName.Equals(TEXT("ReplaceChildAt"), ESearchCase::IgnoreCase);
	}

	static bool IsInputCall(const FString& FunctionName)
	{
		return FunctionName.Contains(TEXT("Input"))
			|| FunctionName.Contains(TEXT("Focus"))
			|| FunctionName.Contains(TEXT("CaptureMouse"))
			|| FunctionName.Contains(TEXT("SetKeyboardFocus"));
	}

	static FString ClassifyFunctionCall(const UFunction* Function)
	{
		if (!Function)
		{
			return TEXT("general");
		}

		const FString FunctionName = Function->GetName();
		if (IsAnimationCall(FunctionName))
		{
			return TEXT("animation");
		}
		if (IsTimingCall(FunctionName, Function->HasMetaData(TEXT("Latent"))))
		{
			return IsTimerCall(FunctionName) ? TEXT("timer") : TEXT("timing");
		}
		if (IsVisibilityCall(FunctionName))
		{
			return TEXT("visibility");
		}
		if (IsUiStateCall(FunctionName))
		{
			return TEXT("ui_state");
		}
		if (IsWidgetTreeCall(FunctionName))
		{
			return TEXT("widget_tree");
		}
		if (IsInputCall(FunctionName))
		{
			return TEXT("input");
		}
		return TEXT("general");
	}

	static void UpdateAggregateSignal(FWidgetLogicAggregate& Aggregate, const FString& CallCategory)
	{
		if (CallCategory == TEXT("animation"))
		{
			Aggregate.bUsesWidgetAnimations = true;
			++Aggregate.AnimationCallCount;
		}
		else if (CallCategory == TEXT("timer"))
		{
			Aggregate.bUsesTiming = true;
			++Aggregate.TimerCallCount;
		}
		else if (CallCategory == TEXT("timing"))
		{
			Aggregate.bUsesTiming = true;
			++Aggregate.LatentCallCount;
		}
		else if (CallCategory == TEXT("visibility"))
		{
			Aggregate.bUsesVisibilityMutation = true;
			++Aggregate.VisibilityCallCount;
		}
		else if (CallCategory == TEXT("ui_state"))
		{
			Aggregate.bUsesUiStateMutation = true;
			++Aggregate.UiStateCallCount;
		}
		else if (CallCategory == TEXT("widget_tree"))
		{
			Aggregate.bUsesWidgetTreeMutation = true;
			++Aggregate.WidgetTreeCallCount;
		}
		else if (CallCategory == TEXT("input"))
		{
			Aggregate.bUsesInputEvents = true;
			++Aggregate.InputCallCount;
		}
	}

	static TSharedPtr<FJsonObject> BuildEntryPointObject(UEdGraphNode* Node, const FString& GraphName)
	{
		const TSharedPtr<FJsonObject> EntryObject = MakeShared<FJsonObject>();
		const FString EventName = Node->IsA<UK2Node_CustomEvent>()
			? CastChecked<UK2Node_CustomEvent>(Node)->CustomFunctionName.ToString()
			: (Node->IsA<UK2Node_Event>() ? CastChecked<UK2Node_Event>(Node)->EventReference.GetMemberName().ToString() : TEXT(""));

		EntryObject->SetStringField(TEXT("graph"), GraphName);
		EntryObject->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
		EntryObject->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
		EntryObject->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		EntryObject->SetStringField(TEXT("event_name"), EventName);
		EntryObject->SetStringField(TEXT("entry_type"), DescribeEntryType(Node, EventName));
		EntryObject->SetNumberField(TEXT("pos_x"), Node->NodePosX);
		EntryObject->SetNumberField(TEXT("pos_y"), Node->NodePosY);
		return EntryObject;
	}

	static TSharedPtr<FJsonObject> BuildCallObject(UEdGraphNode* Node, const FString& GraphName, const UFunction* Function, const FString& CallCategory)
	{
		const TSharedPtr<FJsonObject> CallObject = MakeShared<FJsonObject>();
		const FString FunctionName = Function ? Function->GetName() : TEXT("");
		const FString FunctionClass = Function && Function->GetOuterUClass() ? Function->GetOuterUClass()->GetPathName() : TEXT("");

		CallObject->SetStringField(TEXT("graph"), GraphName);
		CallObject->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
		CallObject->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
		CallObject->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		CallObject->SetStringField(TEXT("call_category"), CallCategory);
		CallObject->SetStringField(TEXT("function_name"), FunctionName);
		CallObject->SetStringField(TEXT("function_class"), FunctionClass);
		CallObject->SetBoolField(TEXT("is_latent"), Function ? Function->HasMetaData(TEXT("Latent")) : false);
		CallObject->SetNumberField(TEXT("pos_x"), Node->NodePosX);
		CallObject->SetNumberField(TEXT("pos_y"), Node->NodePosY);

		if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
		{
			const FString AnimationReference = GetAnimationReferenceFromCallNode(CallNode);
			if (!AnimationReference.IsEmpty())
			{
				CallObject->SetStringField(TEXT("animation_reference"), AnimationReference);
			}
		}

		return CallObject;
	}

	static TSharedPtr<FJsonObject> BuildFunctionGraphMeta(UBlueprint* Blueprint, UEdGraph* Graph)
	{
		const TSharedPtr<FJsonObject> FunctionObject = MakeShared<FJsonObject>();
		FunctionObject->SetStringField(TEXT("name"), Graph ? Graph->GetName() : TEXT(""));
		FunctionObject->SetStringField(TEXT("graph_guid"), Graph ? Graph->GraphGuid.ToString() : TEXT(""));
		FunctionObject->SetNumberField(TEXT("node_count"), Graph ? Graph->Nodes.Num() : 0);
		FunctionObject->SetBoolField(TEXT("is_pure"), false);
		FunctionObject->SetBoolField(TEXT("is_override"), false);

		TArray<TSharedPtr<FJsonObject>> InputParams;
		TArray<TSharedPtr<FJsonObject>> OutputParams;
		TArray<TSharedPtr<FJsonObject>> LocalVariables;

		if (Graph)
		{
			TArray<UK2Node_FunctionEntry*> EntryNodes;
			Graph->GetNodesOfClass(EntryNodes);
			if (EntryNodes.Num() > 0 && EntryNodes[0])
			{
				UK2Node_FunctionEntry* EntryNode = EntryNodes[0];
				FunctionObject->SetBoolField(TEXT("is_pure"), EntryNode->HasAnyExtraFlags(FUNC_BlueprintPure));

				for (UEdGraphPin* Pin : EntryNode->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Output || Pin->PinName == UEdGraphSchema_K2::PN_Then)
					{
						continue;
					}

					const TSharedPtr<FJsonObject> ParamObject = MakeShared<FJsonObject>();
					ParamObject->SetStringField(TEXT("name"), Pin->PinName.ToString());
					ParamObject->SetStringField(TEXT("type"), UEdGraphSchema_K2::TypeToText(Pin->PinType).ToString());
					ParamObject->SetBoolField(TEXT("is_reference"), Pin->PinType.bIsReference);
					ParamObject->SetStringField(TEXT("default_value"), Pin->DefaultValue);
					InputParams.Add(ParamObject);
				}

				for (const FBPVariableDescription& VarDesc : EntryNode->LocalVariables)
				{
					const TSharedPtr<FJsonObject> VariableObject = MakeShared<FJsonObject>();
					VariableObject->SetStringField(TEXT("name"), VarDesc.VarName.ToString());
					VariableObject->SetStringField(TEXT("type"), UEdGraphSchema_K2::TypeToText(VarDesc.VarType).ToString());
					VariableObject->SetStringField(TEXT("friendly_name"), VarDesc.FriendlyName);
					VariableObject->SetStringField(TEXT("category"), VarDesc.Category.ToString());
					LocalVariables.Add(VariableObject);
				}
			}

			TArray<UK2Node_FunctionResult*> ResultNodes;
			Graph->GetNodesOfClass(ResultNodes);
			if (ResultNodes.Num() > 0 && ResultNodes[0])
			{
				for (UEdGraphPin* Pin : ResultNodes[0]->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Input || Pin->PinName == UEdGraphSchema_K2::PN_Execute)
					{
						continue;
					}

					const TSharedPtr<FJsonObject> ParamObject = MakeShared<FJsonObject>();
					ParamObject->SetStringField(TEXT("name"), Pin->PinName.ToString());
					ParamObject->SetStringField(TEXT("type"), UEdGraphSchema_K2::TypeToText(Pin->PinType).ToString());
					ParamObject->SetBoolField(TEXT("is_reference"), Pin->PinType.bIsReference);
					ParamObject->SetStringField(TEXT("default_value"), Pin->DefaultValue);
					OutputParams.Add(ParamObject);
				}
			}
		}

		if (Blueprint && Blueprint->GeneratedClass && Graph)
		{
			if (UFunction* Function = Blueprint->GeneratedClass->FindFunctionByName(FName(*Graph->GetName())))
			{
				FunctionObject->SetBoolField(TEXT("is_override"), Function->GetSuperFunction() != nullptr);
			}
		}

		AddJsonObjectArrayField(FunctionObject, TEXT("input_parameters"), InputParams);
		AddJsonObjectArrayField(FunctionObject, TEXT("output_parameters"), OutputParams);
		AddJsonObjectArrayField(FunctionObject, TEXT("local_variables"), LocalVariables);
		return FunctionObject;
	}

	static TSharedPtr<FJsonObject> BuildGraphSummary(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const FString& Scope,
		int32 MaxCallsPerGraph,
		bool bIncludeNodeClassInventory,
		FWidgetLogicAggregate& Aggregate)
	{
		const TSharedPtr<FJsonObject> GraphObject = MakeShared<FJsonObject>();
		GraphObject->SetStringField(TEXT("name"), Graph ? Graph->GetName() : TEXT(""));
		GraphObject->SetStringField(TEXT("scope"), Scope);
		GraphObject->SetStringField(TEXT("graph_guid"), Graph ? Graph->GraphGuid.ToString() : TEXT(""));
		GraphObject->SetNumberField(TEXT("node_count"), Graph ? Graph->Nodes.Num() : 0);

		if (!Graph)
		{
			return GraphObject;
		}

		TMap<FString, int32> GraphNodeClassCounts;
		TMap<FString, int32> GraphCallCategoryCounts;
		TMap<FString, int32> GraphFunctionCalls;
		TArray<TSharedPtr<FJsonObject>> EntryPoints;
		TArray<TSharedPtr<FJsonObject>> KeyCalls;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			const FString NodeClassName = Node->GetClass()->GetName();
			++Aggregate.TotalNodes;
			++Aggregate.NodeClassCounts.FindOrAdd(NodeClassName);
			++GraphNodeClassCounts.FindOrAdd(NodeClassName);

			if (Scope == TEXT("event") && (Node->IsA<UK2Node_CustomEvent>() || Node->IsA<UK2Node_Event>()))
			{
				const TSharedPtr<FJsonObject> EntryObject = BuildEntryPointObject(Node, Graph->GetName());
				EntryPoints.Add(EntryObject);
				++Aggregate.EntryPointCount;

				const FString EntryType = GetJsonStringField(EntryObject, TEXT("entry_type"));
				if (EntryType == TEXT("tick"))
				{
					Aggregate.bUsesTick = true;
					Aggregate.bUsesLifecycleEvents = true;
				}
				else if (EntryType == TEXT("lifecycle"))
				{
					Aggregate.bUsesLifecycleEvents = true;
				}
				else if (EntryType == TEXT("widget_event"))
				{
					Aggregate.bUsesInputEvents = true;
				}
			}

			if (IsControlFlowNode(NodeClassName))
			{
				Aggregate.bUsesControlFlow = true;
			}

			if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
			{
				const UFunction* Function = CallNode->GetTargetFunction();
				const FString CallCategory = ClassifyFunctionCall(Function);
				const FString FunctionName = Function ? Function->GetName() : Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
				const FString FunctionKey = Function && Function->GetOuterUClass()
					? FString::Printf(TEXT("%s::%s"), *Function->GetOuterUClass()->GetName(), *FunctionName)
					: FunctionName;

				++Aggregate.FunctionCallCount;
				++Aggregate.CallCategoryCounts.FindOrAdd(CallCategory);
				++Aggregate.FunctionCallCounts.FindOrAdd(FunctionKey);
				++GraphCallCategoryCounts.FindOrAdd(CallCategory);
				++GraphFunctionCalls.FindOrAdd(FunctionKey);
				UpdateAggregateSignal(Aggregate, CallCategory);

				const bool bInterestingCall = CallCategory != TEXT("general");
				if (bInterestingCall && KeyCalls.Num() < MaxCallsPerGraph)
				{
					KeyCalls.Add(BuildCallObject(Node, Graph->GetName(), Function, CallCategory));
				}
			}
		}

		AddJsonObjectArrayField(GraphObject, TEXT("entry_points"), EntryPoints);
		AddJsonObjectArrayField(GraphObject, TEXT("key_calls"), KeyCalls);
		AddJsonObjectArrayField(GraphObject, TEXT("call_category_counts"), BuildSortedCountObjects(GraphCallCategoryCounts));
		AddJsonObjectArrayField(GraphObject, TEXT("top_function_calls"), BuildSortedCountObjects(GraphFunctionCalls, 20));
		if (bIncludeNodeClassInventory)
		{
			AddJsonObjectArrayField(GraphObject, TEXT("node_class_counts"), BuildSortedCountObjects(GraphNodeClassCounts, 30));
		}

		return GraphObject;
	}

	static TSharedPtr<FJsonObject> BuildAnimationObject(UWidgetAnimation* Animation)
	{
		const TSharedPtr<FJsonObject> AnimationObject = MakeShared<FJsonObject>();
		AnimationObject->SetStringField(TEXT("name"), Animation ? Animation->GetName() : TEXT(""));
		AnimationObject->SetStringField(TEXT("path"), Animation ? Animation->GetPathName() : TEXT(""));
		return AnimationObject;
	}

	static TSharedPtr<FJsonObject> BuildSignalsObject(const FWidgetLogicAggregate& Aggregate, int32 WidgetAnimationCount)
	{
		const TSharedPtr<FJsonObject> Signals = MakeShared<FJsonObject>();
		Signals->SetBoolField(TEXT("uses_tick"), Aggregate.bUsesTick);
		Signals->SetBoolField(TEXT("uses_lifecycle_events"), Aggregate.bUsesLifecycleEvents);
		Signals->SetBoolField(TEXT("uses_widget_animations"), Aggregate.bUsesWidgetAnimations || WidgetAnimationCount > 0);
		Signals->SetBoolField(TEXT("uses_timing"), Aggregate.bUsesTiming);
		Signals->SetBoolField(TEXT("uses_widget_tree_mutation"), Aggregate.bUsesWidgetTreeMutation);
		Signals->SetBoolField(TEXT("uses_visibility_mutation"), Aggregate.bUsesVisibilityMutation);
		Signals->SetBoolField(TEXT("uses_ui_state_mutation"), Aggregate.bUsesUiStateMutation);
		Signals->SetBoolField(TEXT("uses_input_events"), Aggregate.bUsesInputEvents);
		Signals->SetBoolField(TEXT("uses_control_flow"), Aggregate.bUsesControlFlow);
		return Signals;
	}

	static TSharedPtr<FJsonObject> BuildWidgetLogicSummaryObject(UWidgetBlueprint* WidgetBlueprint, const FString& CanonicalWidgetPath, int32 MaxCallsPerGraph, bool bIncludeNodeClassInventory)
	{
		FWidgetLogicAggregate Aggregate;
		const TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
		RootObject->SetNumberField(TEXT("spec_version"), 1);
		RootObject->SetStringField(TEXT("kind"), TEXT("widget_logic_summary"));

		const TSharedPtr<FJsonObject> AssetObject = MakeShared<FJsonObject>();
		AssetObject->SetStringField(TEXT("widget_path"), GetPackagePathFromAnyWidgetPath(CanonicalWidgetPath));
		AssetObject->SetStringField(TEXT("object_path"), CanonicalWidgetPath);
		AssetObject->SetStringField(TEXT("widget_name"), WidgetBlueprint ? WidgetBlueprint->GetName() : FPackageName::GetLongPackageAssetName(GetPackagePathFromAnyWidgetPath(CanonicalWidgetPath)));
		AssetObject->SetStringField(TEXT("parent_class"), WidgetBlueprint && WidgetBlueprint->ParentClass ? WidgetBlueprint->ParentClass->GetPathName() : TEXT("/Script/UMG.UserWidget"));
		RootObject->SetObjectField(TEXT("asset"), AssetObject);

		TArray<TSharedPtr<FJsonObject>> AnimationObjects;
		if (WidgetBlueprint)
		{
			for (UWidgetAnimation* Animation : WidgetBlueprint->Animations)
			{
				if (!Animation)
				{
					continue;
				}
				AnimationObjects.Add(BuildAnimationObject(Animation));
			}
		}

		TArray<TSharedPtr<FJsonObject>> EventGraphs;
		TArray<TSharedPtr<FJsonObject>> FunctionGraphs;
		TArray<TSharedPtr<FJsonObject>> MacroGraphs;

		if (WidgetBlueprint)
		{
			for (UEdGraph* Graph : WidgetBlueprint->UbergraphPages)
			{
				if (!Graph)
				{
					continue;
				}
				++Aggregate.EventGraphCount;
				EventGraphs.Add(BuildGraphSummary(WidgetBlueprint, Graph, TEXT("event"), MaxCallsPerGraph, bIncludeNodeClassInventory, Aggregate));
			}

			for (UEdGraph* Graph : WidgetBlueprint->FunctionGraphs)
			{
				if (!Graph)
				{
					continue;
				}
				++Aggregate.FunctionGraphCount;
				const TSharedPtr<FJsonObject> FunctionObject = BuildGraphSummary(WidgetBlueprint, Graph, TEXT("function"), MaxCallsPerGraph, bIncludeNodeClassInventory, Aggregate);
				FunctionObject->SetObjectField(TEXT("signature"), BuildFunctionGraphMeta(WidgetBlueprint, Graph));
				FunctionGraphs.Add(FunctionObject);
			}

			for (UEdGraph* Graph : WidgetBlueprint->MacroGraphs)
			{
				if (!Graph)
				{
					continue;
				}
				++Aggregate.MacroGraphCount;
				MacroGraphs.Add(BuildGraphSummary(WidgetBlueprint, Graph, TEXT("macro"), MaxCallsPerGraph, bIncludeNodeClassInventory, Aggregate));
			}
		}

		RootObject->SetArrayField(TEXT("widget_animations"), [&AnimationObjects]()
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Reserve(AnimationObjects.Num());
			for (const TSharedPtr<FJsonObject>& Object : AnimationObjects)
			{
				Values.Add(MakeShared<FJsonValueObject>(Object));
			}
			return Values;
		}());

		const TSharedPtr<FJsonObject> GraphInventory = MakeShared<FJsonObject>();
		GraphInventory->SetNumberField(TEXT("event_graph_count"), Aggregate.EventGraphCount);
		GraphInventory->SetNumberField(TEXT("function_graph_count"), Aggregate.FunctionGraphCount);
		GraphInventory->SetNumberField(TEXT("macro_graph_count"), Aggregate.MacroGraphCount);
		GraphInventory->SetNumberField(TEXT("total_graph_count"), Aggregate.EventGraphCount + Aggregate.FunctionGraphCount + Aggregate.MacroGraphCount);
		GraphInventory->SetNumberField(TEXT("total_node_count"), Aggregate.TotalNodes);
		GraphInventory->SetNumberField(TEXT("entry_point_count"), Aggregate.EntryPointCount);
		GraphInventory->SetNumberField(TEXT("function_call_count"), Aggregate.FunctionCallCount);
		GraphInventory->SetNumberField(TEXT("widget_animation_count"), AnimationObjects.Num());
		RootObject->SetObjectField(TEXT("graph_inventory"), GraphInventory);

		const TSharedPtr<FJsonObject> SummaryObject = MakeShared<FJsonObject>();
		SummaryObject->SetObjectField(TEXT("signals"), BuildSignalsObject(Aggregate, AnimationObjects.Num()));
		AddJsonObjectArrayField(SummaryObject, TEXT("node_class_counts"), BuildSortedCountObjects(Aggregate.NodeClassCounts, 30));
		AddJsonObjectArrayField(SummaryObject, TEXT("call_category_counts"), BuildSortedCountObjects(Aggregate.CallCategoryCounts));
		AddJsonObjectArrayField(SummaryObject, TEXT("top_function_calls"), BuildSortedCountObjects(Aggregate.FunctionCallCounts, 40));
		RootObject->SetObjectField(TEXT("summary"), SummaryObject);

		RootObject->SetArrayField(TEXT("event_graphs"), [&EventGraphs]()
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Reserve(EventGraphs.Num());
			for (const TSharedPtr<FJsonObject>& Object : EventGraphs)
			{
				Values.Add(MakeShared<FJsonValueObject>(Object));
			}
			return Values;
		}());

		RootObject->SetArrayField(TEXT("function_graphs"), [&FunctionGraphs]()
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Reserve(FunctionGraphs.Num());
			for (const TSharedPtr<FJsonObject>& Object : FunctionGraphs)
			{
				Values.Add(MakeShared<FJsonValueObject>(Object));
			}
			return Values;
		}());

		RootObject->SetArrayField(TEXT("macro_graphs"), [&MacroGraphs]()
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Reserve(MacroGraphs.Num());
			for (const TSharedPtr<FJsonObject>& Object : MacroGraphs)
			{
				Values.Add(MakeShared<FJsonValueObject>(Object));
			}
			return Values;
		}());

		return RootObject;
	}

	static void AppendMarkdownLine(FString& Markdown, const FString& Line = TEXT(""))
	{
		Markdown += Line;
		Markdown += TEXT("\n");
	}

	static FString BuildCountLine(const TSharedPtr<FJsonObject>& CountObject)
	{
		if (!CountObject.IsValid())
		{
			return FString();
		}

		return FString::Printf(
			TEXT("- `%s`: %d"),
			*SanitizeInlineMarkdownText(GetJsonStringField(CountObject, TEXT("name"))),
			static_cast<int32>(CountObject->GetNumberField(TEXT("count")))
		);
	}

	static FString BuildWidgetLogicReviewMarkdown(const TSharedPtr<FJsonObject>& LogicObject, const FString& LogicPath)
	{
		FString Markdown;
		const TSharedPtr<FJsonObject>* AssetObject = nullptr;
		LogicObject->TryGetObjectField(TEXT("asset"), AssetObject);
		const TSharedPtr<FJsonObject> AssetJson = AssetObject ? *AssetObject : TSharedPtr<FJsonObject>();

		AppendMarkdownLine(Markdown, FString::Printf(TEXT("# Widget Logic Review: `%s`"), *SanitizeInlineMarkdownText(GetJsonStringField(AssetJson, TEXT("widget_path")))));
		AppendMarkdownLine(Markdown);
		AppendMarkdownLine(Markdown, TEXT("## Asset"));
		AppendMarkdownLine(Markdown, FString::Printf(TEXT("- Widget Path: `%s`"), *SanitizeInlineMarkdownText(GetJsonStringField(AssetJson, TEXT("widget_path")))));
		AppendMarkdownLine(Markdown, FString::Printf(TEXT("- Parent Class: `%s`"), *SanitizeInlineMarkdownText(GetJsonStringField(AssetJson, TEXT("parent_class")))));
		AppendMarkdownLine(Markdown, FString::Printf(TEXT("- Canonical Logic: `%s`"), *SanitizeInlineMarkdownText(LogicPath)));
		AppendMarkdownLine(Markdown);

		const TSharedPtr<FJsonObject>* InventoryObject = nullptr;
		LogicObject->TryGetObjectField(TEXT("graph_inventory"), InventoryObject);
		AppendMarkdownLine(Markdown, TEXT("## Graph Inventory"));
		if (InventoryObject && InventoryObject->IsValid())
		{
			AppendMarkdownLine(Markdown, FString::Printf(TEXT("- Event Graphs: `%d`"), static_cast<int32>((*InventoryObject)->GetNumberField(TEXT("event_graph_count")))));
			AppendMarkdownLine(Markdown, FString::Printf(TEXT("- Function Graphs: `%d`"), static_cast<int32>((*InventoryObject)->GetNumberField(TEXT("function_graph_count")))));
			AppendMarkdownLine(Markdown, FString::Printf(TEXT("- Macro Graphs: `%d`"), static_cast<int32>((*InventoryObject)->GetNumberField(TEXT("macro_graph_count")))));
			AppendMarkdownLine(Markdown, FString::Printf(TEXT("- Total Nodes: `%d`"), static_cast<int32>((*InventoryObject)->GetNumberField(TEXT("total_node_count")))));
			AppendMarkdownLine(Markdown, FString::Printf(TEXT("- Entry Points: `%d`"), static_cast<int32>((*InventoryObject)->GetNumberField(TEXT("entry_point_count")))));
			AppendMarkdownLine(Markdown, FString::Printf(TEXT("- Widget Animations: `%d`"), static_cast<int32>((*InventoryObject)->GetNumberField(TEXT("widget_animation_count")))));
		}
		AppendMarkdownLine(Markdown);

		AppendMarkdownLine(Markdown, TEXT("## Widget Animations"));
		const TArray<TSharedPtr<FJsonValue>>* AnimationArray = nullptr;
		if (LogicObject->TryGetArrayField(TEXT("widget_animations"), AnimationArray) && AnimationArray && AnimationArray->Num() > 0)
		{
			for (const TSharedPtr<FJsonValue>& Value : *AnimationArray)
			{
				const TSharedPtr<FJsonObject> AnimationObject = Value->AsObject();
				AppendMarkdownLine(Markdown, FString::Printf(TEXT("- `%s`"), *GetJsonStringField(AnimationObject, TEXT("name"))));
			}
		}
		else
		{
			AppendMarkdownLine(Markdown, TEXT("- None"));
		}
		AppendMarkdownLine(Markdown);

		const TSharedPtr<FJsonObject>* SummaryObject = nullptr;
		LogicObject->TryGetObjectField(TEXT("summary"), SummaryObject);
		const TSharedPtr<FJsonObject>* SignalsObject = nullptr;
		if (SummaryObject && SummaryObject->IsValid())
		{
			(*SummaryObject)->TryGetObjectField(TEXT("signals"), SignalsObject);
		}

		AppendMarkdownLine(Markdown, TEXT("## Signals"));
		if (SignalsObject && SignalsObject->IsValid())
		{
			for (const FString& FieldName : {
				TEXT("uses_tick"),
				TEXT("uses_lifecycle_events"),
				TEXT("uses_widget_animations"),
				TEXT("uses_timing"),
				TEXT("uses_widget_tree_mutation"),
				TEXT("uses_visibility_mutation"),
				TEXT("uses_ui_state_mutation"),
				TEXT("uses_input_events"),
				TEXT("uses_control_flow")
			})
			{
				AppendMarkdownLine(Markdown, FString::Printf(TEXT("- `%s`: `%s`"), *FieldName, (*SignalsObject)->GetBoolField(FieldName) ? TEXT("true") : TEXT("false")));
			}
		}
		AppendMarkdownLine(Markdown);

		AppendMarkdownLine(Markdown, TEXT("## Call Categories"));
		const TArray<TSharedPtr<FJsonValue>>* CallCategoryArray = nullptr;
		if (SummaryObject && SummaryObject->IsValid() && (*SummaryObject)->TryGetArrayField(TEXT("call_category_counts"), CallCategoryArray) && CallCategoryArray && CallCategoryArray->Num() > 0)
		{
			for (const TSharedPtr<FJsonValue>& Value : *CallCategoryArray)
			{
				AppendMarkdownLine(Markdown, BuildCountLine(Value->AsObject()));
			}
		}
		else
		{
			AppendMarkdownLine(Markdown, TEXT("- None"));
		}
		AppendMarkdownLine(Markdown);

		AppendMarkdownLine(Markdown, TEXT("## Top Function Calls"));
		const TArray<TSharedPtr<FJsonValue>>* TopFunctionCalls = nullptr;
		if (SummaryObject && SummaryObject->IsValid() && (*SummaryObject)->TryGetArrayField(TEXT("top_function_calls"), TopFunctionCalls) && TopFunctionCalls && TopFunctionCalls->Num() > 0)
		{
			for (const TSharedPtr<FJsonValue>& Value : *TopFunctionCalls)
			{
				AppendMarkdownLine(Markdown, BuildCountLine(Value->AsObject()));
			}
		}
		else
		{
			AppendMarkdownLine(Markdown, TEXT("- None"));
		}
		AppendMarkdownLine(Markdown);

		auto AppendGraphsSection = [&Markdown](const FString& Title, const TArray<TSharedPtr<FJsonValue>>* GraphValues, bool bIncludeSignature)
		{
			AppendMarkdownLine(Markdown, FString::Printf(TEXT("## %s"), *Title));
			if (!GraphValues || GraphValues->Num() == 0)
			{
				AppendMarkdownLine(Markdown, TEXT("- None"));
				AppendMarkdownLine(Markdown);
				return;
			}

			for (const TSharedPtr<FJsonValue>& GraphValue : *GraphValues)
			{
				const TSharedPtr<FJsonObject> GraphObject = GraphValue->AsObject();
				AppendMarkdownLine(Markdown, FString::Printf(TEXT("### `%s`"), *SanitizeInlineMarkdownText(GetJsonStringField(GraphObject, TEXT("name")))));
				AppendMarkdownLine(Markdown, FString::Printf(TEXT("- Scope: `%s`"), *SanitizeInlineMarkdownText(GetJsonStringField(GraphObject, TEXT("scope")))));
				AppendMarkdownLine(Markdown, FString::Printf(TEXT("- Nodes: `%d`"), static_cast<int32>(GraphObject->GetNumberField(TEXT("node_count")))));

				if (bIncludeSignature)
				{
					const TSharedPtr<FJsonObject>* SignatureObject = nullptr;
					if (GraphObject->TryGetObjectField(TEXT("signature"), SignatureObject) && SignatureObject && SignatureObject->IsValid())
					{
						AppendMarkdownLine(Markdown, FString::Printf(TEXT("- Pure: `%s`"), (*SignatureObject)->GetBoolField(TEXT("is_pure")) ? TEXT("true") : TEXT("false")));
						AppendMarkdownLine(Markdown, FString::Printf(TEXT("- Override: `%s`"), (*SignatureObject)->GetBoolField(TEXT("is_override")) ? TEXT("true") : TEXT("false")));
					}
				}

				const TArray<TSharedPtr<FJsonValue>>* EntryPoints = nullptr;
				if (GraphObject->TryGetArrayField(TEXT("entry_points"), EntryPoints) && EntryPoints && EntryPoints->Num() > 0)
				{
					AppendMarkdownLine(Markdown, TEXT("- Entry Points:"));
					for (const TSharedPtr<FJsonValue>& EntryValue : *EntryPoints)
					{
						const TSharedPtr<FJsonObject> EntryObject = EntryValue->AsObject();
						FString EntryLabel = GetJsonStringField(EntryObject, TEXT("event_name"));
						if (EntryLabel.IsEmpty())
						{
							EntryLabel = GetJsonStringField(EntryObject, TEXT("node_title"));
						}
						AppendMarkdownLine(
							Markdown,
							FString::Printf(
								TEXT("  - `%s` (%s)"),
								*SanitizeInlineMarkdownText(EntryLabel),
								*SanitizeInlineMarkdownText(GetJsonStringField(EntryObject, TEXT("entry_type")))
							)
						);
					}
				}

				const TArray<TSharedPtr<FJsonValue>>* KeyCalls = nullptr;
				if (GraphObject->TryGetArrayField(TEXT("key_calls"), KeyCalls) && KeyCalls && KeyCalls->Num() > 0)
				{
					AppendMarkdownLine(Markdown, TEXT("- Key Calls:"));
					for (const TSharedPtr<FJsonValue>& CallValue : *KeyCalls)
					{
						const TSharedPtr<FJsonObject> CallObject = CallValue->AsObject();
						const FString AnimationReference = SanitizeInlineMarkdownText(GetJsonStringField(CallObject, TEXT("animation_reference")));
						const FString Suffix = AnimationReference.IsEmpty()
							? TEXT("")
							: FString::Printf(TEXT(" -> `%s`"), *AnimationReference);
						AppendMarkdownLine(
							Markdown,
							FString::Printf(
								TEXT("  - `%s` [%s]%s"),
								*SanitizeInlineMarkdownText(GetJsonStringField(CallObject, TEXT("function_name"))),
								*SanitizeInlineMarkdownText(GetJsonStringField(CallObject, TEXT("call_category"))),
								*Suffix
							)
						);
					}
				}

				const TArray<TSharedPtr<FJsonValue>>* CategoryCounts = nullptr;
				if (GraphObject->TryGetArrayField(TEXT("call_category_counts"), CategoryCounts) && CategoryCounts && CategoryCounts->Num() > 0)
				{
					AppendMarkdownLine(Markdown, TEXT("- Call Categories:"));
					for (const TSharedPtr<FJsonValue>& CountValue : *CategoryCounts)
					{
						AppendMarkdownLine(Markdown, FString::Printf(TEXT("  %s"), *BuildCountLine(CountValue->AsObject())));
					}
				}

				AppendMarkdownLine(Markdown);
			}
		};

		const TArray<TSharedPtr<FJsonValue>>* EventGraphs = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* FunctionGraphs = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* MacroGraphs = nullptr;
		LogicObject->TryGetArrayField(TEXT("event_graphs"), EventGraphs);
		LogicObject->TryGetArrayField(TEXT("function_graphs"), FunctionGraphs);
		LogicObject->TryGetArrayField(TEXT("macro_graphs"), MacroGraphs);

		AppendGraphsSection(TEXT("Event Graphs"), EventGraphs, false);
		AppendGraphsSection(TEXT("Function Graphs"), FunctionGraphs, true);
		AppendGraphsSection(TEXT("Macro Graphs"), MacroGraphs, false);
		return Markdown;
	}

	static void GatherWidgetBlueprintAssetsForBatch(const FString& PathFilter, int32 MaxResults, TArray<FAssetData>& OutAssets, int32& OutTruncatedCount)
	{
		OutAssets.Reset();
		OutTruncatedCount = 0;

		FARFilter Filter;
		Filter.ClassPaths.Add(WidgetBlueprintClassPath);
		Filter.ClassPaths.Add(EditorUtilityWidgetBlueprintClassPath);
		Filter.PackagePaths.Add(*PathFilter);
		Filter.bRecursivePaths = true;

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		AssetRegistryModule.Get().GetAssets(Filter, OutAssets);

		OutAssets.Sort([](const FAssetData& Left, const FAssetData& Right)
		{
			return Left.GetObjectPathString() < Right.GetObjectPathString();
		});

		if (MaxResults > 0 && OutAssets.Num() > MaxResults)
		{
			OutTruncatedCount = OutAssets.Num() - MaxResults;
			OutAssets.SetNum(MaxResults);
		}
	}

	static FString HandleExportWidgetLogicSummary(const TMap<FString, FString>& Params)
	{
		const FString RequestedWidgetPath = ExtractParam(Params, TEXT("widget_path"));
		if (RequestedWidgetPath.IsEmpty())
		{
			return SerializeJson(BuildErrorResponse(
				TEXT("export_widget_logic_summary"),
				TEXT("MISSING_WIDGET_PATH"),
				TEXT("The 'widget_path' parameter is required.")
			));
		}

		UWidgetBlueprint* WidgetBlueprint = nullptr;
		FString CanonicalWidgetPath;
		TSharedPtr<FJsonObject> LoadError;
		if (!LoadWidgetBlueprintAsset(RequestedWidgetPath, WidgetBlueprint, CanonicalWidgetPath, LoadError, TEXT("export_widget_logic_summary")))
		{
			return SerializeJson(LoadError);
		}

		const int32 MaxCallsPerGraph = ExtractIntParam(Params, TEXT("max_calls_per_graph"), 30, 1, 200);
		const bool bWriteReview = ExtractBoolParam(Params, TEXT("write_review"), true);
		const bool bIncludeNodeClassInventory = ExtractBoolParam(Params, TEXT("include_node_class_inventory"), true);
		const FString LogicPath = ResolveOutputPath(ExtractParam(Params, TEXT("logic_path")), BuildDefaultLogicPath(CanonicalWidgetPath));
		const FString ReviewPath = ResolveOutputPath(ExtractParam(Params, TEXT("review_path")), BuildDefaultLogicReviewPath(CanonicalWidgetPath));

		const TSharedPtr<FJsonObject> LogicObject = BuildWidgetLogicSummaryObject(WidgetBlueprint, CanonicalWidgetPath, MaxCallsPerGraph, bIncludeNodeClassInventory);
		if (!SaveTextFile(LogicPath, SerializeJson(LogicObject, true)))
		{
			return SerializeJson(BuildErrorResponse(
				TEXT("export_widget_logic_summary"),
				TEXT("LOGIC_WRITE_FAILED"),
				FString::Printf(TEXT("Failed to write widget logic summary file '%s'."), *LogicPath)
			));
		}

		TArray<FString> ChangedItems = { FString::Printf(TEXT("wrote widget logic summary %s"), *LogicPath) };
		TArray<FString> Warnings;
		if (bWriteReview)
		{
			if (SaveTextFile(ReviewPath, BuildWidgetLogicReviewMarkdown(LogicObject, LogicPath)))
			{
				ChangedItems.Add(FString::Printf(TEXT("wrote widget logic review %s"), *ReviewPath));
			}
			else
			{
				Warnings.Add(FString::Printf(TEXT("Failed to write widget logic review '%s'."), *ReviewPath));
			}
		}

		const TSharedPtr<FJsonObject> DataObject = MakeShared<FJsonObject>();
		DataObject->SetStringField(TEXT("widget_path"), GetPackagePathFromAnyWidgetPath(CanonicalWidgetPath));
		DataObject->SetStringField(TEXT("object_path"), CanonicalWidgetPath);
		DataObject->SetStringField(TEXT("logic_path"), LogicPath);
		DataObject->SetStringField(TEXT("review_path"), bWriteReview ? ReviewPath : TEXT(""));
		DataObject->SetNumberField(TEXT("max_calls_per_graph"), MaxCallsPerGraph);
		DataObject->SetBoolField(TEXT("include_node_class_inventory"), bIncludeNodeClassInventory);

		const TSharedPtr<FJsonObject>* InventoryObject = nullptr;
		LogicObject->TryGetObjectField(TEXT("graph_inventory"), InventoryObject);
		if (InventoryObject && InventoryObject->IsValid())
		{
			DataObject->SetNumberField(TEXT("event_graph_count"), (*InventoryObject)->GetNumberField(TEXT("event_graph_count")));
			DataObject->SetNumberField(TEXT("function_graph_count"), (*InventoryObject)->GetNumberField(TEXT("function_graph_count")));
			DataObject->SetNumberField(TEXT("macro_graph_count"), (*InventoryObject)->GetNumberField(TEXT("macro_graph_count")));
			DataObject->SetNumberField(TEXT("entry_point_count"), (*InventoryObject)->GetNumberField(TEXT("entry_point_count")));
			DataObject->SetNumberField(TEXT("widget_animation_count"), (*InventoryObject)->GetNumberField(TEXT("widget_animation_count")));
		}

		return SerializeJson(BuildSuccessResponse(
			TEXT("export_widget_logic_summary"),
			FString::Printf(TEXT("Exported widget logic summary for %s."), *CanonicalWidgetPath),
			DataObject,
			ChangedItems,
			{
				TEXT("Review the generated .logic.review.md file before making assumptions about runtime behavior."),
				TEXT("If the widget still behaves unexpectedly, inspect the listed entry points and key calls first.")
			},
			Warnings,
			{ CanonicalWidgetPath }
		));
	}

	static FString HandleExportWidgetLogicSummaryBatch(const TMap<FString, FString>& Params)
	{
		const FString PathFilter = ExtractParam(Params, TEXT("path_filter"));
		if (PathFilter.IsEmpty())
		{
			return SerializeJson(BuildErrorResponse(
				TEXT("export_widget_logic_summary_batch"),
				TEXT("MISSING_PATH_FILTER"),
				TEXT("The 'path_filter' parameter is required, for example /Game/EasyGameUI/EasyHudBuilder.")
			));
		}

		const int32 MaxCallsPerGraph = ExtractIntParam(Params, TEXT("max_calls_per_graph"), 30, 1, 200);
		const int32 MaxResults = ExtractIntParam(Params, TEXT("max_results"), 2000, 1, 2000);
		const bool bWriteReview = ExtractBoolParam(Params, TEXT("write_review"), true);
		const bool bIncludeNodeClassInventory = ExtractBoolParam(Params, TEXT("include_node_class_inventory"), false);

		TArray<FAssetData> Assets;
		int32 TruncatedCount = 0;
		GatherWidgetBlueprintAssetsForBatch(PathFilter, MaxResults, Assets, TruncatedCount);
		if (Assets.Num() == 0)
		{
			return SerializeJson(BuildErrorResponse(
				TEXT("export_widget_logic_summary_batch"),
				TEXT("NO_WIDGETS_FOUND"),
				FString::Printf(TEXT("No Widget Blueprints were found under '%s'."), *PathFilter)
			));
		}

		const FString LogicOutputDir = ExtractParam(Params, TEXT("logic_output_dir"));
		const FString ReviewOutputDir = ExtractParam(Params, TEXT("review_output_dir"));
		const FString ManifestPath = ResolveOutputPath(ExtractParam(Params, TEXT("manifest_path")), BuildDefaultLogicManifestPath(PathFilter));

		TArray<TSharedPtr<FJsonObject>> ExportObjects;
		TArray<FString> Warnings;
		TArray<FString> AffectedAssets;

		for (const FAssetData& AssetData : Assets)
		{
			const FString CanonicalWidgetPath = AssetData.GetObjectPathString();
			UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(AssetData.GetAsset());
			if (!WidgetBlueprint)
			{
				WidgetBlueprint = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(CanonicalWidgetPath));
			}

			if (!WidgetBlueprint)
			{
				Warnings.Add(FString::Printf(TEXT("Skipped %s because the Widget Blueprint failed to load."), *CanonicalWidgetPath));
				continue;
			}

			const FString LogicPath = LogicOutputDir.IsEmpty()
				? BuildDefaultLogicPath(CanonicalWidgetPath)
				: ResolveOutputPath(FPaths::Combine(LogicOutputDir, MakeWidgetLogicBaseName(CanonicalWidgetPath) + TEXT(".logic.json")), TEXT(""));
			const FString ReviewPath = ReviewOutputDir.IsEmpty()
				? BuildDefaultLogicReviewPath(CanonicalWidgetPath)
				: ResolveOutputPath(FPaths::Combine(ReviewOutputDir, MakeWidgetLogicBaseName(CanonicalWidgetPath) + TEXT(".logic.review.md")), TEXT(""));

			const TSharedPtr<FJsonObject> LogicObject = BuildWidgetLogicSummaryObject(WidgetBlueprint, CanonicalWidgetPath, MaxCallsPerGraph, bIncludeNodeClassInventory);
			if (!SaveTextFile(LogicPath, SerializeJson(LogicObject, true)))
			{
				Warnings.Add(FString::Printf(TEXT("Failed to write logic summary for %s."), *CanonicalWidgetPath));
				continue;
			}

			bool bReviewWritten = false;
			if (bWriteReview)
			{
				bReviewWritten = SaveTextFile(ReviewPath, BuildWidgetLogicReviewMarkdown(LogicObject, LogicPath));
				if (!bReviewWritten)
				{
					Warnings.Add(FString::Printf(TEXT("Failed to write logic review markdown for %s."), *CanonicalWidgetPath));
				}
			}

			const TSharedPtr<FJsonObject>* InventoryObject = nullptr;
			LogicObject->TryGetObjectField(TEXT("graph_inventory"), InventoryObject);
			const TSharedPtr<FJsonObject>* SummaryObject = nullptr;
			LogicObject->TryGetObjectField(TEXT("summary"), SummaryObject);
			const TSharedPtr<FJsonObject>* SignalsObject = nullptr;
			if (SummaryObject && SummaryObject->IsValid())
			{
				(*SummaryObject)->TryGetObjectField(TEXT("signals"), SignalsObject);
			}

			const TSharedPtr<FJsonObject> ExportObject = MakeShared<FJsonObject>();
			ExportObject->SetStringField(TEXT("widget_path"), GetPackagePathFromAnyWidgetPath(CanonicalWidgetPath));
			ExportObject->SetStringField(TEXT("object_path"), CanonicalWidgetPath);
			ExportObject->SetStringField(TEXT("logic_path"), LogicPath);
			ExportObject->SetStringField(TEXT("review_path"), bWriteReview && bReviewWritten ? ReviewPath : TEXT(""));
			ExportObject->SetNumberField(TEXT("event_graph_count"), InventoryObject && InventoryObject->IsValid() ? (*InventoryObject)->GetNumberField(TEXT("event_graph_count")) : 0);
			ExportObject->SetNumberField(TEXT("function_graph_count"), InventoryObject && InventoryObject->IsValid() ? (*InventoryObject)->GetNumberField(TEXT("function_graph_count")) : 0);
			ExportObject->SetNumberField(TEXT("macro_graph_count"), InventoryObject && InventoryObject->IsValid() ? (*InventoryObject)->GetNumberField(TEXT("macro_graph_count")) : 0);
			ExportObject->SetNumberField(TEXT("entry_point_count"), InventoryObject && InventoryObject->IsValid() ? (*InventoryObject)->GetNumberField(TEXT("entry_point_count")) : 0);
			ExportObject->SetNumberField(TEXT("widget_animation_count"), InventoryObject && InventoryObject->IsValid() ? (*InventoryObject)->GetNumberField(TEXT("widget_animation_count")) : 0);

			if (SignalsObject && SignalsObject->IsValid())
			{
				ExportObject->SetBoolField(TEXT("uses_tick"), (*SignalsObject)->GetBoolField(TEXT("uses_tick")));
				ExportObject->SetBoolField(TEXT("uses_widget_animations"), (*SignalsObject)->GetBoolField(TEXT("uses_widget_animations")));
				ExportObject->SetBoolField(TEXT("uses_timing"), (*SignalsObject)->GetBoolField(TEXT("uses_timing")));
				ExportObject->SetBoolField(TEXT("uses_widget_tree_mutation"), (*SignalsObject)->GetBoolField(TEXT("uses_widget_tree_mutation")));
				ExportObject->SetBoolField(TEXT("uses_visibility_mutation"), (*SignalsObject)->GetBoolField(TEXT("uses_visibility_mutation")));
				ExportObject->SetBoolField(TEXT("uses_ui_state_mutation"), (*SignalsObject)->GetBoolField(TEXT("uses_ui_state_mutation")));
			}

			ExportObjects.Add(ExportObject);
			AffectedAssets.Add(CanonicalWidgetPath);
		}

		const TSharedPtr<FJsonObject> ManifestObject = MakeShared<FJsonObject>();
		ManifestObject->SetNumberField(TEXT("spec_version"), 1);
		ManifestObject->SetStringField(TEXT("kind"), TEXT("widget_logic_summary_batch_export"));
		ManifestObject->SetStringField(TEXT("path_filter"), PathFilter);
		ManifestObject->SetNumberField(TEXT("requested_count"), Assets.Num() + TruncatedCount);
		ManifestObject->SetNumberField(TEXT("exported_count"), ExportObjects.Num());
		ManifestObject->SetNumberField(TEXT("truncated_count"), TruncatedCount);
		ManifestObject->SetNumberField(TEXT("max_calls_per_graph"), MaxCallsPerGraph);
		ManifestObject->SetBoolField(TEXT("write_review"), bWriteReview);
		ManifestObject->SetBoolField(TEXT("include_node_class_inventory"), bIncludeNodeClassInventory);
		AddJsonObjectArrayField(ManifestObject, TEXT("exports"), ExportObjects);

		TArray<FString> ChangedItems;
		if (!SaveTextFile(ManifestPath, SerializeJson(ManifestObject, true)))
		{
			Warnings.Add(FString::Printf(TEXT("Failed to write batch logic manifest file '%s'."), *ManifestPath));
		}
		else
		{
			ChangedItems.Add(FString::Printf(TEXT("wrote widget logic batch manifest %s"), *ManifestPath));
		}

		const TSharedPtr<FJsonObject> DataObject = MakeShared<FJsonObject>();
		DataObject->SetStringField(TEXT("path_filter"), PathFilter);
		DataObject->SetStringField(TEXT("manifest_path"), ManifestPath);
		DataObject->SetNumberField(TEXT("requested_count"), Assets.Num() + TruncatedCount);
		DataObject->SetNumberField(TEXT("exported_count"), ExportObjects.Num());
		DataObject->SetNumberField(TEXT("truncated_count"), TruncatedCount);

		return SerializeJson(BuildSuccessResponse(
			TEXT("export_widget_logic_summary_batch"),
			FString::Printf(TEXT("Exported %d widget logic summary file(s) under %s."), ExportObjects.Num(), *PathFilter),
			DataObject,
			ChangedItems,
			{
				TEXT("Start with the generated .logic.review.md files for widgets that own EventGraph behavior."),
				TEXT("Use the batch manifest to spot widgets that actually use animations, timing, or runtime widget creation.")
			},
			Warnings,
			AffectedAssets
		));
	}
}

REGISTER_VIBEUE_TOOL(export_widget_logic_summary,
	"Export a behavior-focused logic summary for a Widget Blueprint or Editor Utility Widget. The summary inventories EventGraph/function/macro graphs, entry points, key function calls, widget animations, and behavior signals such as timing, visibility, UI-state mutation, and widget-tree changes.",
	"Widget",
	TOOL_PARAMS(
		TOOL_PARAM("widget_path", "Widget Blueprint or Editor Utility Widget path to inspect.", "string", true),
		TOOL_PARAM("logic_path", "Optional output path for the canonical .logic.json file. Relative paths are resolved from the project root.", "string", false),
		TOOL_PARAM("review_path", "Optional output path for the human-readable logic review markdown.", "string", false),
		TOOL_PARAM("write_review", "Whether to also emit a .logic.review.md file. Default true.", "boolean", false),
		TOOL_PARAM("max_calls_per_graph", "Maximum interesting function calls to capture per graph. Default 30.", "number", false),
		TOOL_PARAM("include_node_class_inventory", "Whether to include per-graph node class counts. Default true.", "boolean", false)
	),
	{
		return HandleExportWidgetLogicSummary(Params);
	}
);

REGISTER_VIBEUE_TOOL(export_widget_logic_summary_batch,
	"Batch export behavior-focused logic summaries for Widget Blueprints and Editor Utility Widgets under a path filter. Writes .logic.json files, optional .logic.review.md files, and a manifest that helps you spot which widgets actually contain EventGraph behavior, animations, timing, and runtime UI mutations.",
	"Widget",
	TOOL_PARAMS(
		TOOL_PARAM("path_filter", "Package path prefix to scan, for example /Game/EasyGameUI/EasyHudBuilder.", "string", true),
		TOOL_PARAM("manifest_path", "Optional output path for the batch manifest file.", "string", false),
		TOOL_PARAM("logic_output_dir", "Optional output directory for .logic.json files. Defaults to Saved/VibeUE/WidgetLogic.", "string", false),
		TOOL_PARAM("review_output_dir", "Optional output directory for .logic.review.md files. Defaults to Saved/VibeUE/WidgetLogic.", "string", false),
		TOOL_PARAM("write_review", "Whether to emit .logic.review.md files alongside the logic summaries. Default true.", "boolean", false),
		TOOL_PARAM("max_calls_per_graph", "Maximum interesting function calls to capture per graph. Default 30.", "number", false),
		TOOL_PARAM("max_results", "Maximum widgets to export. Default 2000.", "number", false),
		TOOL_PARAM("include_node_class_inventory", "Whether each widget logic summary should include node class counts. Default false for batch mode.", "boolean", false)
	),
	{
		return HandleExportWidgetLogicSummaryBatch(Params);
	}
);
