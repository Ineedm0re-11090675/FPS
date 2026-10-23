// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Core/ToolRegistry.h"
#include "PythonAPI/UAssetDiscoveryService.h"
#include "PythonAPI/UNiagaraEmitterService.h"
#include "PythonAPI/UNiagaraService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EditorAssetLibrary.h"
#include "Json.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"

namespace
{
	static const FTopLevelAssetPath NiagaraSystemClassPath(TEXT("/Script/Niagara.NiagaraSystem"));

	static FString ExtractNiagaraParam(const TMap<FString, FString>& Params, const FString& FieldName)
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

	static bool ExtractNiagaraBoolParam(const TMap<FString, FString>& Params, const FString& FieldName, bool bDefaultValue)
	{
		const FString Value = ExtractNiagaraParam(Params, FieldName);
		if (Value.IsEmpty())
		{
			return bDefaultValue;
		}

		return Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("1"));
	}

	static int32 ExtractNiagaraIntParam(const TMap<FString, FString>& Params, const FString& FieldName, int32 DefaultValue, int32 MinValue = 0, int32 MaxValue = 500)
	{
		const FString Value = ExtractNiagaraParam(Params, FieldName);
		if (Value.IsEmpty())
		{
			return FMath::Clamp(DefaultValue, MinValue, MaxValue);
		}

		return FMath::Clamp(FCString::Atoi(*Value), MinValue, MaxValue);
	}

	static FString SerializeNiagaraJson(const TSharedPtr<FJsonObject>& JsonObj)
	{
		FString JsonString;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
		FJsonSerializer::Serialize(JsonObj.ToSharedRef(), Writer);
		return JsonString;
	}

	static void AddStringArrayField(TSharedPtr<FJsonObject> JsonObj, const FString& FieldName, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		JsonObj->SetArrayField(FieldName, JsonValues);
	}

	static TSharedPtr<FJsonObject> BuildNiagaraError(
		const FString& Action,
		const FString& ErrorCode,
		const FString& ErrorMessage,
		const FString& SystemPath = TEXT(""),
		const TArray<FString>& VerificationHints = {})
	{
		const TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
		ErrorObj->SetBoolField(TEXT("success"), false);
		ErrorObj->SetStringField(TEXT("action"), Action);
		ErrorObj->SetStringField(TEXT("error_code"), ErrorCode);
		ErrorObj->SetStringField(TEXT("error"), ErrorMessage);
		ErrorObj->SetStringField(TEXT("error_message"), ErrorMessage);
		if (!SystemPath.IsEmpty())
		{
			ErrorObj->SetStringField(TEXT("system_path"), SystemPath);
		}
		AddStringArrayField(ErrorObj, TEXT("verification_hints"), VerificationHints);
		return ErrorObj;
	}

	static TSharedPtr<FJsonObject> BuildNiagaraSuccess(
		const FString& Action,
		const FString& Summary,
		const FString& SystemPath = TEXT(""),
		TSharedPtr<FJsonObject> Data = nullptr,
		const TArray<FString>& ChangedItems = {},
		const TArray<FString>& VerificationHints = {},
		const TArray<FString>& Warnings = {},
		const TArray<FString>& AffectedAssets = {})
	{
		const TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
		RootObj->SetBoolField(TEXT("success"), true);
		RootObj->SetStringField(TEXT("action"), Action);
		RootObj->SetStringField(TEXT("summary"), Summary);
		if (!SystemPath.IsEmpty())
		{
			RootObj->SetStringField(TEXT("system_path"), SystemPath);
		}
		if (Data.IsValid())
		{
			RootObj->SetObjectField(TEXT("data"), Data);
		}
		AddStringArrayField(RootObj, TEXT("changed_items"), ChangedItems);
		AddStringArrayField(RootObj, TEXT("verification_hints"), VerificationHints);
		AddStringArrayField(RootObj, TEXT("warnings"), Warnings);
		AddStringArrayField(RootObj, TEXT("affected_assets"), AffectedAssets);
		return RootObj;
	}

	static FString NormalizeNiagaraAction(const FString& RawAction)
	{
		const FString Action = RawAction.ToLower();
		if (Action == TEXT("create_system"))
		{
			return TEXT("create");
		}
		if (Action == TEXT("system_exists"))
		{
			return TEXT("exists");
		}
		if (Action == TEXT("search_emitter_assets"))
		{
			return TEXT("search_emitters");
		}
		if (Action == TEXT("compile_with_results"))
		{
			return TEXT("compile");
		}
		if (Action == TEXT("get_system_info"))
		{
			return TEXT("describe");
		}
		return Action;
	}

	static FString NormalizePackagePath(const FString& InPath, const FString& DefaultPath = TEXT("/Game/VFX"))
	{
		FString PackagePath = InPath;
		PackagePath.ReplaceInline(TEXT("\\"), TEXT("/"));
		PackagePath.TrimStartAndEndInline();
		while (PackagePath.EndsWith(TEXT("/")))
		{
			PackagePath.LeftChopInline(1);
		}
		if (PackagePath.IsEmpty())
		{
			PackagePath = DefaultPath;
		}
		if (!PackagePath.StartsWith(TEXT("/Game")))
		{
			PackagePath = TEXT("/Game/") + PackagePath;
		}
		return PackagePath;
	}

	static bool ResolveSystemAssetData(const FString& SystemPath, FAssetData& OutAssetData)
	{
		if (SystemPath.IsEmpty())
		{
			return false;
		}

		if (!UAssetDiscoveryService::FindAssetByPath(SystemPath, OutAssetData))
		{
			return false;
		}

		return OutAssetData.AssetClassPath == NiagaraSystemClassPath;
	}

	static bool LoadNiagaraSystemAsset(const FString& SystemPath, UNiagaraSystem*& OutSystem, FString& OutCanonicalPath, TSharedPtr<FJsonObject>& OutError, const FString& Action)
	{
		FAssetData AssetData;
		if (!ResolveSystemAssetData(SystemPath, AssetData))
		{
			OutError = BuildNiagaraError(
				Action,
				TEXT("SYSTEM_NOT_FOUND"),
				FString::Printf(TEXT("Niagara system '%s' was not found."), *SystemPath),
				SystemPath,
				{ TEXT("Use action=search_systems to inspect available Niagara systems.") }
			);
			return false;
		}

		OutCanonicalPath = AssetData.GetObjectPathString();
		OutSystem = Cast<UNiagaraSystem>(UEditorAssetLibrary::LoadAsset(OutCanonicalPath));
		if (!OutSystem)
		{
			OutError = BuildNiagaraError(
				Action,
				TEXT("SYSTEM_LOAD_FAILED"),
				FString::Printf(TEXT("Failed to load Niagara system '%s'."), *OutCanonicalPath),
				OutCanonicalPath,
				{ TEXT("Open the asset in the Niagara editor and confirm it is not corrupted.") }
			);
			return false;
		}

		return true;
	}

	static void ApplyPostNiagaraActions(UNiagaraSystem* NiagaraSystem, const FString& SystemPath, const TMap<FString, FString>& Params, TArray<FString>& InOutChangedItems, TArray<FString>& InOutWarnings)
	{
		if (!NiagaraSystem)
		{
			return;
		}

		if (ExtractNiagaraBoolParam(Params, TEXT("compile_after"), false))
		{
			const FNiagaraCompilationResult CompileResult = UNiagaraService::CompileWithResults(SystemPath);
			InOutChangedItems.Add(TEXT("compiled niagara system"));

			if (!CompileResult.bSuccess)
			{
				InOutWarnings.Add(FString::Printf(TEXT("Compile reported %d error(s). Open the Niagara editor and inspect the compiler results."), CompileResult.ErrorCount));
			}
			else if (CompileResult.WarningCount > 0)
			{
				InOutWarnings.Add(FString::Printf(TEXT("Compile reported %d warning(s). Open the Niagara editor and inspect the compiler results."), CompileResult.WarningCount));
			}
		}

		if (ExtractNiagaraBoolParam(Params, TEXT("save_after"), false))
		{
			if (UNiagaraService::SaveSystem(SystemPath))
			{
				InOutChangedItems.Add(TEXT("saved niagara system"));
			}
			else
			{
				InOutWarnings.Add(TEXT("Failed to save the Niagara system after applying changes."));
			}
		}

		if (ExtractNiagaraBoolParam(Params, TEXT("open_after"), false))
		{
			if (UNiagaraService::OpenInEditor(SystemPath))
			{
				InOutChangedItems.Add(TEXT("opened niagara editor"));
			}
			else
			{
				InOutWarnings.Add(TEXT("Failed to open the Niagara system in the editor."));
			}
		}
	}

	static TSharedPtr<FJsonObject> BuildNiagaraAssetMeta(UNiagaraSystem* NiagaraSystem, const FString& SystemPath)
	{
		const TSharedPtr<FJsonObject> MetaObj = MakeShared<FJsonObject>();
		MetaObj->SetStringField(TEXT("system_path"), SystemPath);
		MetaObj->SetStringField(TEXT("system_name"), NiagaraSystem ? NiagaraSystem->GetName() : TEXT(""));
		MetaObj->SetBoolField(TEXT("is_ready_to_run"), NiagaraSystem ? NiagaraSystem->IsReadyToRun() : false);
		MetaObj->SetBoolField(TEXT("is_valid"), NiagaraSystem ? NiagaraSystem->IsValid() : false);
		MetaObj->SetNumberField(TEXT("emitter_count"), UNiagaraService::ListEmitters(SystemPath).Num());
		return MetaObj;
	}

	static TSharedPtr<FJsonObject> AssetDataToJson(const FAssetData& AssetData)
	{
		const TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
		AssetObj->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		AssetObj->SetStringField(TEXT("asset_class"), AssetData.AssetClassPath.GetAssetName().ToString());
		AssetObj->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
		AssetObj->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
		AssetObj->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
		return AssetObj;
	}

	static TSharedPtr<FJsonObject> NiagaraCreateResultToJson(const FNiagaraCreateResult& Result)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetBoolField(TEXT("success"), Result.bSuccess);
		JsonObj->SetStringField(TEXT("asset_path"), Result.AssetPath);
		JsonObj->SetStringField(TEXT("error_message"), Result.ErrorMessage);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> NiagaraCompilationToJson(const FNiagaraCompilationResult& Result)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetBoolField(TEXT("success"), Result.bSuccess);
		JsonObj->SetStringField(TEXT("system_path"), Result.SystemPath);
		JsonObj->SetNumberField(TEXT("error_count"), Result.ErrorCount);
		JsonObj->SetNumberField(TEXT("warning_count"), Result.WarningCount);
		JsonObj->SetNumberField(TEXT("compilation_time_seconds"), Result.CompilationTimeSeconds);
		AddStringArrayField(JsonObj, TEXT("errors"), Result.Errors);
		AddStringArrayField(JsonObj, TEXT("warnings"), Result.Warnings);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> NiagaraEmitterInfoToJson(const FNiagaraEmitterInfo_Custom& Info)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetStringField(TEXT("emitter_name"), Info.EmitterName);
		JsonObj->SetStringField(TEXT("unique_emitter_name"), Info.UniqueEmitterName);
		JsonObj->SetStringField(TEXT("source_asset_path"), Info.SourceAssetPath);
		JsonObj->SetBoolField(TEXT("is_enabled"), Info.bIsEnabled);
		JsonObj->SetBoolField(TEXT("is_isolated"), Info.bIsIsolated);
		JsonObj->SetStringField(TEXT("sim_target"), Info.SimTarget);
		JsonObj->SetNumberField(TEXT("parameter_count"), Info.ParameterCount);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> NiagaraParameterToJson(const FNiagaraParameterInfo_Custom& Info)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetStringField(TEXT("parameter_name"), Info.ParameterName);
		JsonObj->SetStringField(TEXT("parameter_type"), Info.ParameterType);
		JsonObj->SetStringField(TEXT("namespace"), Info.Namespace);
		JsonObj->SetStringField(TEXT("current_value"), Info.CurrentValue);
		JsonObj->SetStringField(TEXT("default_value"), Info.DefaultValue);
		JsonObj->SetBoolField(TEXT("is_overridden"), Info.bIsOverridden);
		JsonObj->SetBoolField(TEXT("is_user_exposed"), Info.bIsUserExposed);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> NiagaraSummaryToJson(const FNiagaraSystemSummary& Summary)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetStringField(TEXT("system_path"), Summary.SystemPath);
		JsonObj->SetStringField(TEXT("system_name"), Summary.SystemName);
		JsonObj->SetNumberField(TEXT("emitter_count"), Summary.EmitterCount);
		JsonObj->SetNumberField(TEXT("user_parameter_count"), Summary.UserParameterCount);
		JsonObj->SetBoolField(TEXT("has_gpu_emitters"), Summary.bHasGPUEmitters);
		AddStringArrayField(JsonObj, TEXT("emitter_names"), Summary.EmitterNames);
		AddStringArrayField(JsonObj, TEXT("user_parameter_names"), Summary.UserParameterNames);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> NiagaraSystemInfoToJson(const FNiagaraSystemInfo_Custom& Info)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetStringField(TEXT("system_name"), Info.SystemName);
		JsonObj->SetStringField(TEXT("system_path"), Info.SystemPath);
		JsonObj->SetBoolField(TEXT("is_valid"), Info.bIsValid);
		JsonObj->SetNumberField(TEXT("emitter_count"), Info.EmitterCount);
		JsonObj->SetBoolField(TEXT("needs_recompile"), Info.bNeedsRecompile);
		JsonObj->SetStringField(TEXT("fixed_bounds"), Info.FixedBounds);

		TArray<TSharedPtr<FJsonValue>> EmittersArray;
		EmittersArray.Reserve(Info.Emitters.Num());
		for (const FNiagaraEmitterInfo_Custom& Emitter : Info.Emitters)
		{
			EmittersArray.Add(MakeShared<FJsonValueObject>(NiagaraEmitterInfoToJson(Emitter)));
		}
		JsonObj->SetArrayField(TEXT("emitters"), EmittersArray);

		TArray<TSharedPtr<FJsonValue>> ParametersArray;
		ParametersArray.Reserve(Info.UserParameters.Num());
		for (const FNiagaraParameterInfo_Custom& Parameter : Info.UserParameters)
		{
			ParametersArray.Add(MakeShared<FJsonValueObject>(NiagaraParameterToJson(Parameter)));
		}
		JsonObj->SetArrayField(TEXT("user_parameters"), ParametersArray);

		return JsonObj;
	}

	static TSharedPtr<FJsonObject> NiagaraSystemPropertiesToJson(const FNiagaraSystemPropertiesInfo& Info)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetStringField(TEXT("system_name"), Info.SystemName);
		JsonObj->SetStringField(TEXT("system_path"), Info.SystemPath);
		JsonObj->SetNumberField(TEXT("emitter_count"), Info.EmitterCount);
		JsonObj->SetStringField(TEXT("effect_type_name"), Info.EffectTypeName);
		JsonObj->SetStringField(TEXT("effect_type_path"), Info.EffectTypePath);
		JsonObj->SetStringField(TEXT("cull_reaction"), Info.CullReaction);
		JsonObj->SetStringField(TEXT("update_frequency"), Info.UpdateFrequency);
		JsonObj->SetBoolField(TEXT("determinism"), Info.bDeterminism);
		JsonObj->SetNumberField(TEXT("random_seed"), Info.RandomSeed);
		JsonObj->SetNumberField(TEXT("warmup_time"), Info.WarmupTime);
		JsonObj->SetNumberField(TEXT("warmup_tick_count"), Info.WarmupTickCount);
		JsonObj->SetNumberField(TEXT("warmup_tick_delta"), Info.WarmupTickDelta);
		JsonObj->SetBoolField(TEXT("fixed_bounds_enabled"), Info.bFixedBounds);
		JsonObj->SetStringField(TEXT("fixed_bounds_value"), Info.FixedBoundsValue);
		JsonObj->SetBoolField(TEXT("support_large_world_coordinates"), Info.bSupportLargeWorldCoordinates);
		JsonObj->SetBoolField(TEXT("cast_shadow"), Info.bCastShadow);
		JsonObj->SetBoolField(TEXT("receives_decals"), Info.bReceivesDecals);
		JsonObj->SetBoolField(TEXT("render_custom_depth"), Info.bRenderCustomDepth);
		JsonObj->SetNumberField(TEXT("translucency_sort_priority"), Info.TranslucencySortPriority);
		JsonObj->SetBoolField(TEXT("bake_out_rapid_iteration"), Info.bBakeOutRapidIteration);
		JsonObj->SetBoolField(TEXT("compress_attributes"), Info.bCompressAttributes);
		JsonObj->SetBoolField(TEXT("trim_attributes"), Info.bTrimAttributes);
		JsonObj->SetBoolField(TEXT("override_scalability_settings"), Info.bOverrideScalabilitySettings);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> NiagaraModuleInputToJson(const FNiagaraModuleInputInfo& Info)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetStringField(TEXT("input_name"), Info.InputName);
		JsonObj->SetStringField(TEXT("input_type"), Info.InputType);
		JsonObj->SetStringField(TEXT("current_value"), Info.CurrentValue);
		JsonObj->SetStringField(TEXT("default_value"), Info.DefaultValue);
		JsonObj->SetBoolField(TEXT("is_linked"), Info.bIsLinked);
		JsonObj->SetStringField(TEXT("linked_source"), Info.LinkedSource);
		JsonObj->SetBoolField(TEXT("is_editable"), Info.bIsEditable);
		JsonObj->SetStringField(TEXT("input_source"), Info.InputSource);
		JsonObj->SetBoolField(TEXT("is_exposed"), Info.bIsExposed);
		JsonObj->SetStringField(TEXT("bound_class"), Info.BoundClass);
		JsonObj->SetStringField(TEXT("bound_asset_path"), Info.BoundAssetPath);
		JsonObj->SetStringField(TEXT("bound_user_parameter"), Info.BoundUserParameter);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> NiagaraModuleInfoToJson(const FNiagaraModuleInfo_Custom& Info)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetStringField(TEXT("module_name"), Info.ModuleName);
		JsonObj->SetStringField(TEXT("module_type"), Info.ModuleType);
		JsonObj->SetStringField(TEXT("script_asset_path"), Info.ScriptAssetPath);
		JsonObj->SetNumberField(TEXT("module_index"), Info.ModuleIndex);
		JsonObj->SetBoolField(TEXT("is_enabled"), Info.bIsEnabled);

		TArray<TSharedPtr<FJsonValue>> InputsArray;
		InputsArray.Reserve(Info.Inputs.Num());
		for (const FNiagaraModuleInputInfo& Input : Info.Inputs)
		{
			InputsArray.Add(MakeShared<FJsonValueObject>(NiagaraModuleInputToJson(Input)));
		}
		JsonObj->SetArrayField(TEXT("inputs"), InputsArray);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> NiagaraRendererInfoToJson(const FNiagaraRendererInfo_Custom& Info)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetStringField(TEXT("renderer_name"), Info.RendererName);
		JsonObj->SetStringField(TEXT("renderer_type"), Info.RendererType);
		JsonObj->SetNumberField(TEXT("renderer_index"), Info.RendererIndex);
		JsonObj->SetBoolField(TEXT("is_enabled"), Info.bIsEnabled);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> NiagaraRendererDetailsToJson(const FNiagaraRendererDetailedInfo& Info)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetStringField(TEXT("renderer_name"), Info.RendererName);
		JsonObj->SetStringField(TEXT("renderer_type"), Info.RendererType);
		JsonObj->SetNumberField(TEXT("renderer_index"), Info.RendererIndex);
		JsonObj->SetBoolField(TEXT("is_enabled"), Info.bIsEnabled);
		JsonObj->SetStringField(TEXT("material_path"), Info.MaterialPath);
		JsonObj->SetBoolField(TEXT("has_material"), Info.bHasMaterial);
		JsonObj->SetStringField(TEXT("sub_image_size"), Info.SubImageSize);
		JsonObj->SetStringField(TEXT("alignment"), Info.Alignment);
		JsonObj->SetStringField(TEXT("facing_mode"), Info.FacingMode);
		JsonObj->SetStringField(TEXT("sort_mode"), Info.SortMode);
		JsonObj->SetStringField(TEXT("mesh_path"), Info.MeshPath);
		JsonObj->SetStringField(TEXT("ribbon_shape"), Info.RibbonShape);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> NiagaraRIParameterToJson(const FNiagaraRIParameterInfo& Info)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetStringField(TEXT("parameter_name"), Info.ParameterName);
		JsonObj->SetStringField(TEXT("parameter_type"), Info.ParameterType);
		JsonObj->SetStringField(TEXT("value"), Info.Value);
		JsonObj->SetStringField(TEXT("script_type"), Info.ScriptType);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> NiagaraEmitterLifecycleToJson(const FNiagaraEmitterLifecycleInfo& Info)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetStringField(TEXT("emitter_name"), Info.EmitterName);
		JsonObj->SetBoolField(TEXT("is_enabled"), Info.bIsEnabled);
		JsonObj->SetStringField(TEXT("life_cycle_mode"), Info.LifeCycleMode);
		JsonObj->SetStringField(TEXT("loop_behavior"), Info.LoopBehavior);
		JsonObj->SetNumberField(TEXT("loop_count"), Info.LoopCount);
		JsonObj->SetNumberField(TEXT("loop_duration"), Info.LoopDuration);
		JsonObj->SetNumberField(TEXT("loop_delay"), Info.LoopDelay);
		JsonObj->SetBoolField(TEXT("duration_recalc_each_loop"), Info.bDurationRecalcEachLoop);
		JsonObj->SetBoolField(TEXT("delay_recalc_each_loop"), Info.bDelayRecalcEachLoop);
		JsonObj->SetBoolField(TEXT("inactive_from_start"), Info.bInactiveFromStart);
		JsonObj->SetStringField(TEXT("scalability_mode"), Info.ScalabilityMode);
		JsonObj->SetNumberField(TEXT("ri_parameter_count"), Info.RIParameterCount);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> NiagaraEmitterPropertiesToJson(const FNiagaraEmitterPropertiesInfo& Info)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetStringField(TEXT("emitter_name"), Info.EmitterName);
		JsonObj->SetBoolField(TEXT("is_enabled"), Info.bIsEnabled);
		JsonObj->SetStringField(TEXT("sim_target"), Info.SimTarget);
		JsonObj->SetBoolField(TEXT("local_space"), Info.bLocalSpace);
		JsonObj->SetBoolField(TEXT("determinism"), Info.bDeterminism);
		JsonObj->SetNumberField(TEXT("random_seed"), Info.RandomSeed);
		JsonObj->SetStringField(TEXT("calculate_bounds_mode"), Info.CalculateBoundsMode);
		JsonObj->SetStringField(TEXT("fixed_bounds"), Info.FixedBounds);
		JsonObj->SetStringField(TEXT("allocation_mode"), Info.AllocationMode);
		JsonObj->SetNumberField(TEXT("pre_allocation_count"), Info.PreAllocationCount);
		JsonObj->SetStringField(TEXT("loop_behavior"), Info.LoopBehavior);
		JsonObj->SetStringField(TEXT("loop_duration"), Info.LoopDuration);
		JsonObj->SetStringField(TEXT("inactive_response"), Info.InactiveResponse);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> NiagaraScriptInfoToJson(const FNiagaraScriptInfo_Custom& Info)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetStringField(TEXT("script_name"), Info.ScriptName);
		JsonObj->SetStringField(TEXT("script_path"), Info.ScriptPath);
		JsonObj->SetStringField(TEXT("script_usage"), Info.ScriptUsage);
		JsonObj->SetStringField(TEXT("description"), Info.Description);
		AddStringArrayField(JsonObj, TEXT("input_names"), Info.InputNames);
		AddStringArrayField(JsonObj, TEXT("output_names"), Info.OutputNames);
		return JsonObj;
	}

	static TArray<TSharedPtr<FJsonValue>> AssetDataArrayToJson(const TArray<FAssetData>& Assets)
	{
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		JsonArray.Reserve(Assets.Num());
		for (const FAssetData& Asset : Assets)
		{
			JsonArray.Add(MakeShared<FJsonValueObject>(AssetDataToJson(Asset)));
		}
		return JsonArray;
	}

	static TArray<TSharedPtr<FJsonValue>> EmitterInfoArrayToJson(const TArray<FNiagaraEmitterInfo_Custom>& Infos)
	{
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		JsonArray.Reserve(Infos.Num());
		for (const FNiagaraEmitterInfo_Custom& Info : Infos)
		{
			JsonArray.Add(MakeShared<FJsonValueObject>(NiagaraEmitterInfoToJson(Info)));
		}
		return JsonArray;
	}

	static TArray<TSharedPtr<FJsonValue>> NiagaraParameterArrayToJson(const TArray<FNiagaraParameterInfo_Custom>& Infos)
	{
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		JsonArray.Reserve(Infos.Num());
		for (const FNiagaraParameterInfo_Custom& Info : Infos)
		{
			JsonArray.Add(MakeShared<FJsonValueObject>(NiagaraParameterToJson(Info)));
		}
		return JsonArray;
	}

	static TArray<TSharedPtr<FJsonValue>> NiagaraModuleArrayToJson(const TArray<FNiagaraModuleInfo_Custom>& Infos)
	{
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		JsonArray.Reserve(Infos.Num());
		for (const FNiagaraModuleInfo_Custom& Info : Infos)
		{
			JsonArray.Add(MakeShared<FJsonValueObject>(NiagaraModuleInfoToJson(Info)));
		}
		return JsonArray;
	}

	static TArray<TSharedPtr<FJsonValue>> NiagaraRendererArrayToJson(const TArray<FNiagaraRendererInfo_Custom>& Infos)
	{
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		JsonArray.Reserve(Infos.Num());
		for (const FNiagaraRendererInfo_Custom& Info : Infos)
		{
			JsonArray.Add(MakeShared<FJsonValueObject>(NiagaraRendererInfoToJson(Info)));
		}
		return JsonArray;
	}

	static TArray<TSharedPtr<FJsonValue>> NiagaraRIParameterArrayToJson(const TArray<FNiagaraRIParameterInfo>& Infos)
	{
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		JsonArray.Reserve(Infos.Num());
		for (const FNiagaraRIParameterInfo& Info : Infos)
		{
			JsonArray.Add(MakeShared<FJsonValueObject>(NiagaraRIParameterToJson(Info)));
		}
		return JsonArray;
	}

	static TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		JsonArray.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonArray.Add(MakeShared<FJsonValueString>(Value));
		}
		return JsonArray;
	}

	static FString BuildNiagaraHelpResponse()
	{
		const TSharedPtr<FJsonObject> HelpObj = MakeShared<FJsonObject>();
		HelpObj->SetBoolField(TEXT("success"), true);
		HelpObj->SetStringField(TEXT("tool"), TEXT("niagara_system"));
		HelpObj->SetStringField(TEXT("description"), TEXT("Structured Niagara system editing and inspection. Covers system lifecycle, emitter management, user parameters, module stacks, renderers, and rapid iteration parameters."));

		TArray<TSharedPtr<FJsonValue>> ActionsArray;
		auto AddAction = [&ActionsArray](const FString& Name, const FString& Desc)
		{
			const TSharedPtr<FJsonObject> ActionObj = MakeShared<FJsonObject>();
			ActionObj->SetStringField(TEXT("action"), Name);
			ActionObj->SetStringField(TEXT("description"), Desc);
			ActionsArray.Add(MakeShared<FJsonValueObject>(ActionObj));
		};

		AddAction(TEXT("help"), TEXT("Show this help payload."));
		AddAction(TEXT("search_systems"), TEXT("Search Niagara systems under a path/name filter."));
		AddAction(TEXT("search_emitters"), TEXT("Search standalone emitter assets."));
		AddAction(TEXT("list_emitter_templates"), TEXT("List emitter templates that can be added to a system."));
		AddAction(TEXT("search_module_scripts"), TEXT("Search Niagara module scripts by name/type."));
		AddAction(TEXT("list_builtin_modules"), TEXT("List common built-in Niagara modules."));
		AddAction(TEXT("create"), TEXT("Create a Niagara system, optionally from a system/emitter template."));
		AddAction(TEXT("exists"), TEXT("Check whether a Niagara system exists."));
		AddAction(TEXT("describe"), TEXT("Inspect a Niagara system and optionally one emitter in detail."));
		AddAction(TEXT("open"), TEXT("Open a Niagara system in the editor."));
		AddAction(TEXT("save"), TEXT("Save a Niagara system."));
		AddAction(TEXT("compile"), TEXT("Compile a Niagara system and return compiler diagnostics."));
		AddAction(TEXT("list_emitters"), TEXT("List emitters in a system."));
		AddAction(TEXT("add_emitter"), TEXT("Add a minimal or templated emitter to a system."));
		AddAction(TEXT("copy_emitter"), TEXT("Copy an emitter from one system to another."));
		AddAction(TEXT("remove_emitter"), TEXT("Remove an emitter from a system."));
		AddAction(TEXT("enable_emitter"), TEXT("Enable or disable an emitter."));
		AddAction(TEXT("duplicate_emitter"), TEXT("Duplicate an emitter in the same system."));
		AddAction(TEXT("rename_emitter"), TEXT("Rename an emitter."));
		AddAction(TEXT("move_emitter"), TEXT("Reorder an emitter in the system."));
		AddAction(TEXT("list_parameters"), TEXT("List user-exposed system parameters."));
		AddAction(TEXT("get_parameter"), TEXT("Read a system parameter."));
		AddAction(TEXT("set_parameter"), TEXT("Set a user/system rapid-iteration parameter by name."));
		AddAction(TEXT("add_user_parameter"), TEXT("Add a user-exposed parameter."));
		AddAction(TEXT("remove_user_parameter"), TEXT("Remove a user-exposed parameter."));
		AddAction(TEXT("list_modules"), TEXT("List modules in an emitter."));
		AddAction(TEXT("get_module_info"), TEXT("Inspect one emitter module."));
		AddAction(TEXT("add_module"), TEXT("Add a module script to an emitter stack."));
		AddAction(TEXT("remove_module"), TEXT("Remove a module from an emitter."));
		AddAction(TEXT("enable_module"), TEXT("Enable or disable a module."));
		AddAction(TEXT("set_module_input"), TEXT("Set a module input value. Supports visible graph pins and hidden clipboard-backed inputs."));
		AddAction(TEXT("get_module_input"), TEXT("Read a module input pin value."));
		AddAction(TEXT("set_module_di_user_parameter"), TEXT("Bind a module's internal DataInterface or hidden linked/object input to a Niagara user parameter."));
		AddAction(TEXT("list_renderers"), TEXT("List renderers on an emitter."));
		AddAction(TEXT("get_renderer_details"), TEXT("Inspect one renderer including material details."));
		AddAction(TEXT("add_renderer"), TEXT("Add a renderer to an emitter."));
		AddAction(TEXT("remove_renderer"), TEXT("Remove a renderer from an emitter."));
		AddAction(TEXT("enable_renderer"), TEXT("Enable or disable a renderer."));
		AddAction(TEXT("set_renderer_property"), TEXT("Set a renderer property via reflection."));
		AddAction(TEXT("list_rapid_iteration_params"), TEXT("List rapid iteration parameters for one emitter."));
		AddAction(TEXT("set_rapid_iteration_param"), TEXT("Set a rapid iteration parameter across matching stages."));
		AddAction(TEXT("set_rapid_iteration_param_by_stage"), TEXT("Set a rapid iteration parameter in a specific stage."));
		AddAction(TEXT("get_emitter_lifecycle"), TEXT("Inspect loop/lifecycle settings for an emitter."));
		AddAction(TEXT("get_emitter_properties"), TEXT("Inspect emitter simulation/allocation/bounds properties."));

		HelpObj->SetArrayField(TEXT("actions"), ActionsArray);

		TArray<FString> Notes;
		Notes.Add(TEXT("Mutating actions support compile_after, save_after, and open_after booleans."));
		Notes.Add(TEXT("Use describe with emitter_name to ground Codex/Cursor in modules, renderers, and rapid iteration params before editing."));
		Notes.Add(TEXT("Spawn rate, lifetime, Init Particle, Color, and similar controls are typically changed via set_module_input or set_rapid_iteration_param(_by_stage)."));
		Notes.Add(TEXT("set_module_input now also supports hidden clipboard-backed inputs. Use prefixes like link:, local:, expr:, asset:, or reset/default when you need to override the inferred input mode."));
		Notes.Add(TEXT("Some built-in modules hide DataInterface/ObjectAsset/linked inputs behind stack clipboard data rather than regular pins. get_module_info now exposes these via clipboard-backed input_source entries when possible."));
		AddStringArrayField(HelpObj, TEXT("notes"), Notes);

		TArray<FString> Examples;
		Examples.Add(TEXT("action=create, system_name=NS_Fire, destination_path=/Game/VFX"));
		Examples.Add(TEXT("action=add_emitter, system_path=/Game/VFX/NS_Fire, emitter_asset_path=minimal, emitter_name=Flames, compile_after=true"));
		Examples.Add(TEXT("action=describe, system_path=/Game/VFX/NS_Fire, emitter_name=Flames"));
		Examples.Add(TEXT("action=add_module, system_path=/Game/VFX/NS_Fire, emitter_name=Flames, module_script_path=/Niagara/Modules/Update/Size/ScaleSpriteSize, module_type=ParticleUpdate"));
		Examples.Add(TEXT("action=set_module_input, system_path=/Game/VFX/NS_Fire, emitter_name=Flames, module_name=Initialize Particle, input_name=Lifetime, value=2.5"));
		Examples.Add(TEXT("action=set_module_input, system_path=/Game/VFX/NS_Fire, emitter_name=Flames, module_name=SamplePseudoVolumeTexture, input_name='UVW Coordinates', value='link:Particles.Position'"));
		Examples.Add(TEXT("action=set_module_di_user_parameter, system_path=/Game/VFX/NS_Fire, emitter_name=Flames, module_name=SampleTexture, input_name=Texture, value=User.SourceMaskTexture"));
		Examples.Add(TEXT("action=set_rapid_iteration_param, system_path=/Game/VFX/NS_Fire, emitter_name=Flames, parameter_name=Constants.Flames.SpawnRate.SpawnRate, value=250.0"));
		AddStringArrayField(HelpObj, TEXT("examples"), Examples);

		return SerializeNiagaraJson(HelpObj);
	}
}

REGISTER_VIBEUE_TOOL(niagara_system,
	"Structured Niagara system editing and inspection. Covers system lifecycle, emitters, user parameters, module stacks, renderers, and rapid iteration parameters. Use action=help for full documentation.",
	"Niagara",
	TOOL_PARAMS(
		TOOL_PARAM("action", "Niagara operation to perform. Use help for documentation.", "string", true),
		TOOL_PARAM("system_path", "Niagara system asset path (package or object path). Required for most actions.", "string", false),
		TOOL_PARAM("search_path", "[search/list] Package path filter such as /Game/VFX", "string", false),
		TOOL_PARAM("filter_text", "[search/list] Optional name filter", "string", false),
		TOOL_PARAM("max_results", "[search/list] Maximum results to return (default 100)", "number", false),
		TOOL_PARAM("system_name", "[create] Niagara system asset name", "string", false),
		TOOL_PARAM("asset_name", "[create] Alias of system_name", "string", false),
		TOOL_PARAM("destination_path", "[create] Destination package path", "string", false),
		TOOL_PARAM("template_asset_path", "[create] Optional Niagara system or emitter template asset path", "string", false),
		TOOL_PARAM("emitter_name", "[emitter actions] Target emitter name", "string", false),
		TOOL_PARAM("emitter_asset_path", "[add_emitter] Emitter asset path, or 'minimal'/'empty' for a minimal emitter", "string", false),
		TOOL_PARAM("source_system_path", "[copy_emitter] Source system path", "string", false),
		TOOL_PARAM("source_emitter_name", "[copy_emitter] Source emitter name", "string", false),
		TOOL_PARAM("target_system_path", "[copy_emitter] Target system path; defaults to system_path", "string", false),
		TOOL_PARAM("new_emitter_name", "[copy/duplicate/rename] New emitter name", "string", false),
		TOOL_PARAM("enabled", "[enable_*] Whether to enable or disable", "boolean", false),
		TOOL_PARAM("new_index", "[move_emitter] Target emitter index", "number", false),
		TOOL_PARAM("parameter_name", "[parameter actions] Parameter name", "string", false),
		TOOL_PARAM("parameter_type", "[add_user_parameter] Type like Float, Int, Bool, Color, Vector, Texture/UTexture, TextureDI/DataInterfaceTexture, MaterialInterface, SkeletalMesh, StaticMesh", "string", false),
		TOOL_PARAM("value", "[set_* actions] New value as string", "string", false),
		TOOL_PARAM("module_name", "[module actions] Module name", "string", false),
		TOOL_PARAM("module_type", "[list/add_module/list_builtin_modules/search_module_scripts] Stack/type filter", "string", false),
		TOOL_PARAM("module_script_path", "[add_module] Module script asset path", "string", false),
		TOOL_PARAM("input_name", "[module input actions] Module input pin name", "string", false),
		TOOL_PARAM("renderer_index", "[renderer actions] Renderer index", "number", false),
		TOOL_PARAM("renderer_type", "[add_renderer] Sprite, Mesh, Ribbon, Light, or Component", "string", false),
		TOOL_PARAM("property_name", "[renderer property] Property name", "string", false),
		TOOL_PARAM("script_type", "[RI or script search] EmitterSpawn, EmitterUpdate, ParticleSpawn, ParticleUpdate, Spawn, Update, or Event", "string", false),
		TOOL_PARAM("script_path", "[get_script_info] Niagara script asset path", "string", false),
		TOOL_PARAM("compile_after", "[mutating actions] Compile the Niagara system after the operation", "boolean", false),
		TOOL_PARAM("save_after", "[mutating actions] Save the Niagara system after the operation", "boolean", false),
		TOOL_PARAM("open_after", "[create/mutating actions] Open the Niagara editor after the operation", "boolean", false)
	),
	{
		const FString Action = NormalizeNiagaraAction(ExtractNiagaraParam(Params, TEXT("action")));
		if (Action.IsEmpty())
		{
			return SerializeNiagaraJson(BuildNiagaraError(TEXT("unknown"), TEXT("MISSING_ACTION"), TEXT("The 'action' parameter is required. Use action=help for documentation.")));
		}

		if (Action == TEXT("help"))
		{
			return BuildNiagaraHelpResponse();
		}

		if (Action == TEXT("search_systems"))
		{
			const FString SearchPath = NormalizePackagePath(ExtractNiagaraParam(Params, TEXT("search_path")), TEXT("/Game"));
			const FString FilterText = ExtractNiagaraParam(Params, TEXT("filter_text"));
			const int32 MaxResults = ExtractNiagaraIntParam(Params, TEXT("max_results"), 100, 1, 500);

			IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			FARFilter Filter;
			Filter.ClassPaths.Add(NiagaraSystemClassPath);
			Filter.PackagePaths.Add(FName(*SearchPath));
			Filter.bRecursivePaths = true;

			TArray<FAssetData> AssetDataList;
			AssetRegistry.GetAssets(Filter, AssetDataList);

			TArray<FAssetData> FilteredAssets;
			for (const FAssetData& AssetData : AssetDataList)
			{
				const FString AssetName = AssetData.AssetName.ToString();
				if (!FilterText.IsEmpty() && !AssetName.Contains(FilterText, ESearchCase::IgnoreCase))
				{
					continue;
				}
				FilteredAssets.Add(AssetData);
			}

			TArray<FAssetData> ClampedAssets;
			const int32 ClampedCount = FMath::Min(MaxResults, FilteredAssets.Num());
			for (int32 Index = 0; Index < ClampedCount; ++Index)
			{
				ClampedAssets.Add(FilteredAssets[Index]);
			}

			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetStringField(TEXT("search_path"), SearchPath);
			DataObj->SetStringField(TEXT("filter_text"), FilterText);
			DataObj->SetNumberField(TEXT("result_count"), FilteredAssets.Num());
			DataObj->SetNumberField(TEXT("truncated_count"), FMath::Max(0, FilteredAssets.Num() - MaxResults));
			DataObj->SetArrayField(TEXT("systems"), AssetDataArrayToJson(ClampedAssets));

			return SerializeNiagaraJson(BuildNiagaraSuccess(Action, FString::Printf(TEXT("Found %d Niagara system(s)."), FilteredAssets.Num()), TEXT(""), DataObj));
		}

		if (Action == TEXT("search_emitters") || Action == TEXT("list_emitter_templates"))
		{
			const FString SearchPath = Action == TEXT("search_emitters")
				? NormalizePackagePath(ExtractNiagaraParam(Params, TEXT("search_path")), TEXT("/Game"))
				: ExtractNiagaraParam(Params, TEXT("search_path"));
			const FString FilterText = ExtractNiagaraParam(Params, TEXT("filter_text"));
			const int32 MaxResults = ExtractNiagaraIntParam(Params, TEXT("max_results"), 100, 1, 500);
			const TArray<FString> RawPaths = Action == TEXT("search_emitters")
				? UNiagaraService::SearchEmitterAssets(SearchPath, FilterText)
				: UNiagaraService::ListEmitterTemplates(SearchPath, FilterText);

			TArray<TSharedPtr<FJsonValue>> JsonArray;
			JsonArray.Reserve(FMath::Min(MaxResults, RawPaths.Num()));
			for (int32 Index = 0; Index < RawPaths.Num() && Index < MaxResults; ++Index)
			{
				JsonArray.Add(MakeShared<FJsonValueString>(RawPaths[Index]));
			}

			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetStringField(TEXT("search_path"), SearchPath);
			DataObj->SetStringField(TEXT("filter_text"), FilterText);
			DataObj->SetNumberField(TEXT("result_count"), RawPaths.Num());
			DataObj->SetNumberField(TEXT("truncated_count"), FMath::Max(0, RawPaths.Num() - MaxResults));
			DataObj->SetArrayField(TEXT("emitters"), JsonArray);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Resolved %d Niagara emitter template(s)."), RawPaths.Num()),
				TEXT(""),
				DataObj
			));
		}

		if (Action == TEXT("search_module_scripts") || Action == TEXT("list_builtin_modules"))
		{
			const FString FilterText = ExtractNiagaraParam(Params, TEXT("filter_text"));
			const FString ModuleType = ExtractNiagaraParam(Params, TEXT("module_type"));
			const int32 MaxResults = ExtractNiagaraIntParam(Params, TEXT("max_results"), 100, 1, 500);
			const TArray<FString> RawPaths = Action == TEXT("search_module_scripts")
				? UNiagaraEmitterService::SearchModuleScripts(FilterText, ModuleType)
				: UNiagaraEmitterService::ListBuiltinModules(ModuleType);

			TArray<TSharedPtr<FJsonValue>> JsonArray;
			JsonArray.Reserve(FMath::Min(MaxResults, RawPaths.Num()));
			for (int32 Index = 0; Index < RawPaths.Num() && Index < MaxResults; ++Index)
			{
				JsonArray.Add(MakeShared<FJsonValueString>(RawPaths[Index]));
			}

			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetStringField(TEXT("filter_text"), FilterText);
			DataObj->SetStringField(TEXT("module_type"), ModuleType);
			DataObj->SetNumberField(TEXT("result_count"), RawPaths.Num());
			DataObj->SetNumberField(TEXT("truncated_count"), FMath::Max(0, RawPaths.Num() - MaxResults));
			DataObj->SetArrayField(TEXT("scripts"), JsonArray);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Resolved %d Niagara module script(s)."), RawPaths.Num()),
				TEXT(""),
				DataObj
			));
		}

		if (Action == TEXT("create"))
		{
			const FString SystemName = ExtractNiagaraParam(Params, TEXT("system_name")).IsEmpty()
				? ExtractNiagaraParam(Params, TEXT("asset_name"))
				: ExtractNiagaraParam(Params, TEXT("system_name"));
			const FString DestinationPath = NormalizePackagePath(ExtractNiagaraParam(Params, TEXT("destination_path")));
			const FString TemplateAssetPath = ExtractNiagaraParam(Params, TEXT("template_asset_path"));

			if (SystemName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_SYSTEM_NAME"), TEXT("The 'system_name' parameter is required for create.")));
			}

			const FNiagaraCreateResult CreateResult = UNiagaraService::CreateSystem(SystemName, DestinationPath, TemplateAssetPath);
			if (!CreateResult.bSuccess)
			{
				return SerializeNiagaraJson(BuildNiagaraError(
					Action,
					TEXT("CREATE_FAILED"),
					CreateResult.ErrorMessage.IsEmpty() ? TEXT("Failed to create Niagara system.") : CreateResult.ErrorMessage
				));
			}

			UNiagaraSystem* NiagaraSystem = nullptr;
			FString CanonicalSystemPath;
			TSharedPtr<FJsonObject> LoadError;
			if (!LoadNiagaraSystemAsset(CreateResult.AssetPath, NiagaraSystem, CanonicalSystemPath, LoadError, Action))
			{
				return SerializeNiagaraJson(LoadError);
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("created niagara system %s"), *CanonicalSystemPath));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			const TSharedPtr<FJsonObject> DataObj = NiagaraCreateResultToJson(CreateResult);
			DataObj->SetObjectField(TEXT("asset"), BuildNiagaraAssetMeta(NiagaraSystem, CanonicalSystemPath));
			DataObj->SetStringField(TEXT("template_asset_path"), TemplateAssetPath);

			TArray<FString> VerificationHints;
			VerificationHints.Add(TEXT("Open the Niagara editor and verify the system overview graph and emitter stack."));
			VerificationHints.Add(TEXT("If you created from an emitter template, inspect the emitter renderer/modules before further edits."));

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Created Niagara system %s."), *CanonicalSystemPath),
				CanonicalSystemPath,
				DataObj,
				ChangedItems,
				VerificationHints,
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("get_script_info"))
		{
			const FString ScriptPath = ExtractNiagaraParam(Params, TEXT("script_path"));
			if (ScriptPath.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_SCRIPT_PATH"), TEXT("The 'script_path' parameter is required for get_script_info.")));
			}

			FNiagaraScriptInfo_Custom ScriptInfo;
			if (!UNiagaraEmitterService::GetScriptInfo(ScriptPath, ScriptInfo))
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("SCRIPT_NOT_FOUND"), FString::Printf(TEXT("Niagara script '%s' was not found."), *ScriptPath)));
			}

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Inspected Niagara script %s."), *ScriptInfo.ScriptName),
				TEXT(""),
				NiagaraScriptInfoToJson(ScriptInfo)
			));
		}

		const FString RequestedSystemPath = ExtractNiagaraParam(Params, TEXT("system_path"));
		if (Action == TEXT("exists"))
		{
			FAssetData AssetData;
			const bool bExists = ResolveSystemAssetData(RequestedSystemPath, AssetData);
			const FString ResolvedPath = bExists ? AssetData.GetObjectPathString() : RequestedSystemPath;
			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetBoolField(TEXT("exists"), bExists);
			DataObj->SetStringField(TEXT("resolved_system_path"), ResolvedPath);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				bExists
					? FString::Printf(TEXT("Niagara system %s exists."), *ResolvedPath)
					: FString::Printf(TEXT("Niagara system %s does not exist."), *RequestedSystemPath),
				ResolvedPath,
				DataObj,
				{},
				{},
				{},
				bExists ? TArray<FString>{ ResolvedPath } : TArray<FString>{}
			));
		}

		if (RequestedSystemPath.IsEmpty())
		{
			return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_SYSTEM_PATH"), TEXT("The 'system_path' parameter is required for this action.")));
		}

		UNiagaraSystem* NiagaraSystem = nullptr;
		FString CanonicalSystemPath;
		TSharedPtr<FJsonObject> LoadError;
		if (!LoadNiagaraSystemAsset(RequestedSystemPath, NiagaraSystem, CanonicalSystemPath, LoadError, Action))
		{
			return SerializeNiagaraJson(LoadError);
		}

		if (Action == TEXT("open"))
		{
			if (!UNiagaraService::OpenInEditor(CanonicalSystemPath))
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("OPEN_FAILED"), FString::Printf(TEXT("Failed to open Niagara system '%s'."), *CanonicalSystemPath), CanonicalSystemPath));
			}

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Opened Niagara system %s."), *CanonicalSystemPath),
				CanonicalSystemPath,
				BuildNiagaraAssetMeta(NiagaraSystem, CanonicalSystemPath),
				{ TEXT("opened niagara editor") },
				{},
				{},
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("save"))
		{
			if (!UNiagaraService::SaveSystem(CanonicalSystemPath))
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("SAVE_FAILED"), FString::Printf(TEXT("Failed to save Niagara system '%s'."), *CanonicalSystemPath), CanonicalSystemPath));
			}

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Saved Niagara system %s."), *CanonicalSystemPath),
				CanonicalSystemPath,
				BuildNiagaraAssetMeta(NiagaraSystem, CanonicalSystemPath),
				{ TEXT("saved niagara system") },
				{},
				{},
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("compile"))
		{
			const FNiagaraCompilationResult CompileResult = UNiagaraService::CompileWithResults(CanonicalSystemPath);
			TArray<FString> ChangedItems;
			ChangedItems.Add(TEXT("compiled niagara system"));

			TArray<FString> Warnings = CompileResult.Warnings;
			if (!CompileResult.bSuccess)
			{
				Warnings.Append(CompileResult.Errors);
			}

			if (ExtractNiagaraBoolParam(Params, TEXT("save_after"), false))
			{
				if (UNiagaraService::SaveSystem(CanonicalSystemPath))
				{
					ChangedItems.Add(TEXT("saved niagara system"));
				}
				else
				{
					Warnings.Add(TEXT("Compile completed but saving the Niagara system failed."));
				}
			}

			const TSharedPtr<FJsonObject> DataObj = NiagaraCompilationToJson(CompileResult);
			DataObj->SetObjectField(TEXT("asset"), BuildNiagaraAssetMeta(NiagaraSystem, CanonicalSystemPath));

			TArray<FString> VerificationHints;
			VerificationHints.Add(TEXT("Open the Niagara editor compiler results and confirm there are no remaining errors/warnings."));
			VerificationHints.Add(TEXT("Preview the system and verify emitter behavior, spawn rate, lifetime, and renderer output."));

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Compiled Niagara system %s (%s, errors=%d, warnings=%d)."), *CanonicalSystemPath, CompileResult.bSuccess ? TEXT("ok") : TEXT("error"), CompileResult.ErrorCount, CompileResult.WarningCount),
				CanonicalSystemPath,
				DataObj,
				ChangedItems,
				VerificationHints,
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("describe"))
		{
			FNiagaraSystemInfo_Custom SystemInfo;
			FNiagaraSystemSummary Summary;
			FNiagaraSystemPropertiesInfo Properties;
			const bool bHasSystemInfo = UNiagaraService::GetSystemInfo(CanonicalSystemPath, SystemInfo);
			const bool bHasSummary = UNiagaraService::Summarize(CanonicalSystemPath, Summary);
			const bool bHasProperties = UNiagaraService::GetSystemProperties(CanonicalSystemPath, Properties);
			const TArray<FNiagaraEmitterInfo_Custom> Emitters = UNiagaraService::ListEmitters(CanonicalSystemPath);
			const TArray<FNiagaraParameterInfo_Custom> Parameters = UNiagaraService::ListParameters(CanonicalSystemPath);

			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetObjectField(TEXT("asset"), BuildNiagaraAssetMeta(NiagaraSystem, CanonicalSystemPath));
			DataObj->SetArrayField(TEXT("emitters"), EmitterInfoArrayToJson(Emitters));
			DataObj->SetArrayField(TEXT("parameters"), NiagaraParameterArrayToJson(Parameters));

			if (bHasSystemInfo)
			{
				DataObj->SetObjectField(TEXT("system_info"), NiagaraSystemInfoToJson(SystemInfo));
			}
			if (bHasSummary)
			{
				DataObj->SetObjectField(TEXT("summary"), NiagaraSummaryToJson(Summary));
			}
			if (bHasProperties)
			{
				DataObj->SetObjectField(TEXT("system_properties"), NiagaraSystemPropertiesToJson(Properties));
			}

			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			if (Action == TEXT("describe") && !EmitterName.IsEmpty() && UNiagaraService::EmitterExists(CanonicalSystemPath, EmitterName))
			{
				FNiagaraEmitterLifecycleInfo LifecycleInfo;
				FNiagaraEmitterPropertiesInfo EmitterProperties;
				const TArray<FNiagaraModuleInfo_Custom> Modules = UNiagaraEmitterService::ListModules(CanonicalSystemPath, EmitterName, TEXT(""));
				const TArray<FNiagaraRendererInfo_Custom> Renderers = UNiagaraEmitterService::ListRenderers(CanonicalSystemPath, EmitterName);
				const TArray<FNiagaraRIParameterInfo> RIParams = UNiagaraService::ListRapidIterationParams(CanonicalSystemPath, EmitterName);

				DataObj->SetStringField(TEXT("focused_emitter"), EmitterName);
				DataObj->SetArrayField(TEXT("modules"), NiagaraModuleArrayToJson(Modules));
				DataObj->SetArrayField(TEXT("renderers"), NiagaraRendererArrayToJson(Renderers));
				DataObj->SetArrayField(TEXT("rapid_iteration_parameters"), NiagaraRIParameterArrayToJson(RIParams));

				if (UNiagaraService::GetEmitterLifecycle(CanonicalSystemPath, EmitterName, LifecycleInfo))
				{
					DataObj->SetObjectField(TEXT("emitter_lifecycle"), NiagaraEmitterLifecycleToJson(LifecycleInfo));
				}
				if (UNiagaraEmitterService::GetEmitterProperties(CanonicalSystemPath, EmitterName, EmitterProperties))
				{
					DataObj->SetObjectField(TEXT("emitter_properties"), NiagaraEmitterPropertiesToJson(EmitterProperties));
				}
			}

			TArray<FString> VerificationHints;
			VerificationHints.Add(TEXT("Open the Niagara editor and verify system overview ordering, emitter enable state, and preview playback."));
			VerificationHints.Add(TEXT("If an emitter is focused, inspect its module stack, rapid iteration parameters, and renderer/material output."));

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Inspected %s (%d emitter(s), %d user parameter(s))."), *CanonicalSystemPath, Emitters.Num(), Parameters.Num()),
				CanonicalSystemPath,
				DataObj,
				{},
				VerificationHints,
				{},
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("emitter_exists"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			if (EmitterName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_EMITTER_NAME"), TEXT("The 'emitter_name' parameter is required for emitter_exists."), CanonicalSystemPath));
			}

			const bool bExists = UNiagaraService::EmitterExists(CanonicalSystemPath, EmitterName);
			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetBoolField(TEXT("exists"), bExists);
			DataObj->SetStringField(TEXT("emitter_name"), EmitterName);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				bExists
					? FString::Printf(TEXT("Emitter %s exists in %s."), *EmitterName, *CanonicalSystemPath)
					: FString::Printf(TEXT("Emitter %s was not found in %s."), *EmitterName, *CanonicalSystemPath),
				CanonicalSystemPath,
				DataObj,
				{},
				bExists ? TArray<FString>{} : TArray<FString>{ TEXT("Use action=describe or action=list_emitters to inspect emitter names.") },
				{},
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("parameter_exists"))
		{
			const FString ParameterName = ExtractNiagaraParam(Params, TEXT("parameter_name"));
			if (ParameterName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETER_NAME"), TEXT("The 'parameter_name' parameter is required for parameter_exists."), CanonicalSystemPath));
			}

			const bool bExists = UNiagaraService::ParameterExists(CanonicalSystemPath, ParameterName);
			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetBoolField(TEXT("exists"), bExists);
			DataObj->SetStringField(TEXT("parameter_name"), ParameterName);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				bExists
					? FString::Printf(TEXT("Parameter %s exists in %s."), *ParameterName, *CanonicalSystemPath)
					: FString::Printf(TEXT("Parameter %s was not found in %s."), *ParameterName, *CanonicalSystemPath),
				CanonicalSystemPath,
				DataObj,
				{},
				bExists ? TArray<FString>{} : TArray<FString>{ TEXT("Use action=list_parameters or action=describe to inspect available parameter names.") },
				{},
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("list_parameters"))
		{
			const TArray<FNiagaraParameterInfo_Custom> Parameters = UNiagaraService::ListParameters(CanonicalSystemPath);
			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetNumberField(TEXT("parameter_count"), Parameters.Num());
			DataObj->SetArrayField(TEXT("parameters"), NiagaraParameterArrayToJson(Parameters));

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Listed %d user parameter(s) for %s."), Parameters.Num(), *CanonicalSystemPath),
				CanonicalSystemPath,
				DataObj,
				{},
				{},
				{},
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("get_parameter"))
		{
			const FString ParameterName = ExtractNiagaraParam(Params, TEXT("parameter_name"));
			if (ParameterName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETER_NAME"), TEXT("The 'parameter_name' parameter is required for get_parameter."), CanonicalSystemPath));
			}

			FNiagaraParameterInfo_Custom ParameterInfo;
			if (!UNiagaraService::GetParameter(CanonicalSystemPath, ParameterName, ParameterInfo))
			{
				return SerializeNiagaraJson(BuildNiagaraError(
					Action,
					TEXT("PARAMETER_NOT_FOUND"),
					FString::Printf(TEXT("Parameter '%s' was not found."), *ParameterName),
					CanonicalSystemPath,
					{ TEXT("Use action=list_parameters, action=list_rapid_iteration_params, or action=describe to inspect valid parameter names.") }
				));
			}

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Read parameter %s."), *ParameterInfo.ParameterName),
				CanonicalSystemPath,
				NiagaraParameterToJson(ParameterInfo),
				{},
				{},
				{},
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("set_parameter"))
		{
			const FString ParameterName = ExtractNiagaraParam(Params, TEXT("parameter_name"));
			const FString Value = ExtractNiagaraParam(Params, TEXT("value"));
			if (ParameterName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETER_NAME"), TEXT("The 'parameter_name' parameter is required for set_parameter."), CanonicalSystemPath));
			}

			if (!UNiagaraService::SetParameter(CanonicalSystemPath, ParameterName, Value))
			{
				return SerializeNiagaraJson(BuildNiagaraError(
					Action,
					TEXT("SET_PARAMETER_FAILED"),
					FString::Printf(TEXT("Failed to set parameter '%s' to '%s'."), *ParameterName, *Value),
					CanonicalSystemPath,
					{ TEXT("Use action=get_parameter or action=list_parameters to verify the exact name and value format.") }
				));
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("set parameter %s = %s"), *ParameterName, *Value));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			FNiagaraParameterInfo_Custom ParameterInfo;
			UNiagaraService::GetParameter(CanonicalSystemPath, ParameterName, ParameterInfo);

			TArray<FString> VerificationHints;
			VerificationHints.Add(TEXT("Open the Niagara editor parameter panel and verify the parameter default/value."));
			VerificationHints.Add(TEXT("Preview the system to confirm the parameter drives the intended visual change."));

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Updated parameter %s in %s."), *ParameterName, *CanonicalSystemPath),
				CanonicalSystemPath,
				NiagaraParameterToJson(ParameterInfo),
				ChangedItems,
				VerificationHints,
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("add_user_parameter"))
		{
			const FString ParameterName = ExtractNiagaraParam(Params, TEXT("parameter_name"));
			const FString ParameterType = ExtractNiagaraParam(Params, TEXT("parameter_type"));
			const FString Value = ExtractNiagaraParam(Params, TEXT("value"));
			if (ParameterName.IsEmpty() || ParameterType.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETERS"), TEXT("Both 'parameter_name' and 'parameter_type' are required for add_user_parameter."), CanonicalSystemPath));
			}

			if (!UNiagaraService::AddUserParameter(CanonicalSystemPath, ParameterName, ParameterType, Value))
			{
				return SerializeNiagaraJson(BuildNiagaraError(
					Action,
					TEXT("ADD_USER_PARAMETER_FAILED"),
					FString::Printf(TEXT("Failed to add user parameter '%s' (%s)."), *ParameterName, *ParameterType),
					CanonicalSystemPath
				));
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("added user parameter %s (%s)"), *ParameterName, *ParameterType));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Added user parameter %s to %s."), *ParameterName, *CanonicalSystemPath),
				CanonicalSystemPath,
				nullptr,
				ChangedItems,
				{
					TEXT("Open the Niagara parameter panel and confirm the new user parameter appears with the expected type."),
					TEXT("If the parameter should be wired into modules or renderers, verify those bindings next.")
				},
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("remove_user_parameter"))
		{
			const FString ParameterName = ExtractNiagaraParam(Params, TEXT("parameter_name"));
			if (ParameterName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETER_NAME"), TEXT("The 'parameter_name' parameter is required for remove_user_parameter."), CanonicalSystemPath));
			}

			if (!UNiagaraService::RemoveUserParameter(CanonicalSystemPath, ParameterName))
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("REMOVE_USER_PARAMETER_FAILED"), FString::Printf(TEXT("Failed to remove user parameter '%s'."), *ParameterName), CanonicalSystemPath));
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("removed user parameter %s"), *ParameterName));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Removed user parameter %s from %s."), *ParameterName, *CanonicalSystemPath),
				CanonicalSystemPath,
				nullptr,
				ChangedItems,
				{
					TEXT("Open the Niagara parameter panel and confirm the parameter is gone."),
					TEXT("Verify that modules/renderers no longer reference the removed parameter.")
				},
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("list_emitters"))
		{
			const TArray<FNiagaraEmitterInfo_Custom> Emitters = UNiagaraService::ListEmitters(CanonicalSystemPath);
			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetNumberField(TEXT("emitter_count"), Emitters.Num());
			DataObj->SetArrayField(TEXT("emitters"), EmitterInfoArrayToJson(Emitters));
			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Listed %d emitter(s) for %s."), Emitters.Num(), *CanonicalSystemPath),
				CanonicalSystemPath,
				DataObj,
				{},
				{},
				{},
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("add_emitter"))
		{
			const FString EmitterAssetPath = ExtractNiagaraParam(Params, TEXT("emitter_asset_path"));
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			const FString AddedEmitterName = UNiagaraService::AddEmitter(
				CanonicalSystemPath,
				EmitterAssetPath.IsEmpty() ? TEXT("minimal") : EmitterAssetPath,
				EmitterName
			);
			if (AddedEmitterName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(
					Action,
					TEXT("ADD_EMITTER_FAILED"),
					FString::Printf(TEXT("Failed to add emitter to %s."), *CanonicalSystemPath),
					CanonicalSystemPath,
					{ TEXT("Use action=list_emitter_templates to inspect valid templates, or pass emitter_asset_path=minimal.") }
				));
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("added emitter %s"), *AddedEmitterName));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Added emitter %s to %s."), *AddedEmitterName, *CanonicalSystemPath),
				CanonicalSystemPath,
				nullptr,
				ChangedItems,
				{
					TEXT("Open the Niagara system overview and verify the emitter stack order and enable state."),
					TEXT("Inspect the new emitter's modules, renderers, and preview behavior before further edits.")
				},
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("copy_emitter"))
		{
			const FString SourceSystemPath = ExtractNiagaraParam(Params, TEXT("source_system_path"));
			const FString SourceEmitterName = ExtractNiagaraParam(Params, TEXT("source_emitter_name"));
			const FString TargetSystemPath = ExtractNiagaraParam(Params, TEXT("target_system_path")).IsEmpty()
				? CanonicalSystemPath
				: ExtractNiagaraParam(Params, TEXT("target_system_path"));
			const FString NewEmitterName = ExtractNiagaraParam(Params, TEXT("new_emitter_name"));
			if (SourceSystemPath.IsEmpty() || SourceEmitterName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETERS"), TEXT("Both 'source_system_path' and 'source_emitter_name' are required for copy_emitter."), CanonicalSystemPath));
			}

			const FString AddedEmitterName = UNiagaraService::CopyEmitter(SourceSystemPath, SourceEmitterName, TargetSystemPath, NewEmitterName);
			if (AddedEmitterName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(
					Action,
					TEXT("COPY_EMITTER_FAILED"),
					FString::Printf(TEXT("Failed to copy emitter '%s' from '%s'."), *SourceEmitterName, *SourceSystemPath),
					CanonicalSystemPath
				));
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("copied emitter %s from %s"), *AddedEmitterName, *SourceSystemPath));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Copied emitter %s into %s."), *AddedEmitterName, *CanonicalSystemPath),
				CanonicalSystemPath,
				nullptr,
				ChangedItems,
				{
					TEXT("Open the Niagara system overview and confirm the copied emitter has the expected modules/renderers."),
					TEXT("Preview the system to ensure the copied emitter behaves correctly in the target context.")
				},
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("remove_emitter"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			if (EmitterName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_EMITTER_NAME"), TEXT("The 'emitter_name' parameter is required for remove_emitter."), CanonicalSystemPath));
			}

			if (!UNiagaraService::RemoveEmitter(CanonicalSystemPath, EmitterName))
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("REMOVE_EMITTER_FAILED"), FString::Printf(TEXT("Failed to remove emitter '%s'."), *EmitterName), CanonicalSystemPath));
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("removed emitter %s"), *EmitterName));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Removed emitter %s from %s."), *EmitterName, *CanonicalSystemPath),
				CanonicalSystemPath,
				nullptr,
				ChangedItems,
				{
					TEXT("Open the Niagara system overview and confirm the emitter stack/order after removal."),
					TEXT("Preview the system to ensure no dependent renderers/events broke when the emitter was removed.")
				},
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("enable_emitter"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			const bool bEnabled = ExtractNiagaraBoolParam(Params, TEXT("enabled"), true);
			if (EmitterName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_EMITTER_NAME"), TEXT("The 'emitter_name' parameter is required for enable_emitter."), CanonicalSystemPath));
			}

			if (!UNiagaraService::EnableEmitter(CanonicalSystemPath, EmitterName, bEnabled))
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("ENABLE_EMITTER_FAILED"), FString::Printf(TEXT("Failed to %s emitter '%s'."), bEnabled ? TEXT("enable") : TEXT("disable"), *EmitterName), CanonicalSystemPath));
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("%s emitter %s"), bEnabled ? TEXT("enabled") : TEXT("disabled"), *EmitterName));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("%s emitter %s in %s."), bEnabled ? TEXT("Enabled") : TEXT("Disabled"), *EmitterName, *CanonicalSystemPath),
				CanonicalSystemPath,
				nullptr,
				ChangedItems,
				{ TEXT("Open the Niagara system overview and verify the emitter enable toggle and preview output.") },
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("duplicate_emitter"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			const FString NewEmitterName = ExtractNiagaraParam(Params, TEXT("new_emitter_name"));
			if (EmitterName.IsEmpty() || NewEmitterName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETERS"), TEXT("Both 'emitter_name' and 'new_emitter_name' are required for duplicate_emitter."), CanonicalSystemPath));
			}

			if (!UNiagaraService::DuplicateEmitter(CanonicalSystemPath, EmitterName, NewEmitterName))
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("DUPLICATE_EMITTER_FAILED"), FString::Printf(TEXT("Failed to duplicate emitter '%s' to '%s'."), *EmitterName, *NewEmitterName), CanonicalSystemPath));
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("duplicated emitter %s to %s"), *EmitterName, *NewEmitterName));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Duplicated emitter %s to %s."), *EmitterName, *NewEmitterName),
				CanonicalSystemPath,
				nullptr,
				ChangedItems,
				{
					TEXT("Open the Niagara system overview and verify the duplicated emitter stack/position."),
					TEXT("Inspect the duplicated emitter for renamed/internal references before further edits.")
				},
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("rename_emitter"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			const FString NewEmitterName = ExtractNiagaraParam(Params, TEXT("new_emitter_name"));
			if (EmitterName.IsEmpty() || NewEmitterName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETERS"), TEXT("Both 'emitter_name' and 'new_emitter_name' are required for rename_emitter."), CanonicalSystemPath));
			}

			if (!UNiagaraService::RenameEmitter(CanonicalSystemPath, EmitterName, NewEmitterName))
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("RENAME_EMITTER_FAILED"), FString::Printf(TEXT("Failed to rename emitter '%s' to '%s'."), *EmitterName, *NewEmitterName), CanonicalSystemPath));
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("renamed emitter %s to %s"), *EmitterName, *NewEmitterName));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Renamed emitter %s to %s."), *EmitterName, *NewEmitterName),
				CanonicalSystemPath,
				nullptr,
				ChangedItems,
				{ TEXT("Open the Niagara system overview and verify the emitter name and any parameter paths that reference it.") },
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("move_emitter"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			const int32 NewIndex = ExtractNiagaraIntParam(Params, TEXT("new_index"), 0, 0, 1024);
			if (EmitterName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_EMITTER_NAME"), TEXT("The 'emitter_name' parameter is required for move_emitter."), CanonicalSystemPath));
			}

			if (!UNiagaraService::MoveEmitter(CanonicalSystemPath, EmitterName, NewIndex))
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MOVE_EMITTER_FAILED"), FString::Printf(TEXT("Failed to move emitter '%s' to index %d."), *EmitterName, NewIndex), CanonicalSystemPath));
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("moved emitter %s to index %d"), *EmitterName, NewIndex));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Moved emitter %s to index %d."), *EmitterName, NewIndex),
				CanonicalSystemPath,
				nullptr,
				ChangedItems,
				{ TEXT("Open the Niagara system overview and confirm emitter ordering and graph layout.") },
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("list_modules"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			const FString ModuleType = ExtractNiagaraParam(Params, TEXT("module_type"));
			if (EmitterName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_EMITTER_NAME"), TEXT("The 'emitter_name' parameter is required for list_modules."), CanonicalSystemPath));
			}

			const TArray<FNiagaraModuleInfo_Custom> Modules = UNiagaraEmitterService::ListModules(CanonicalSystemPath, EmitterName, ModuleType);
			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetStringField(TEXT("emitter_name"), EmitterName);
			DataObj->SetStringField(TEXT("module_type"), ModuleType);
			DataObj->SetNumberField(TEXT("module_count"), Modules.Num());
			DataObj->SetArrayField(TEXT("modules"), NiagaraModuleArrayToJson(Modules));

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Listed %d module(s) for emitter %s."), Modules.Num(), *EmitterName),
				CanonicalSystemPath,
				DataObj,
				{},
				{},
				{},
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("get_module_info"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			const FString ModuleName = ExtractNiagaraParam(Params, TEXT("module_name"));
			if (EmitterName.IsEmpty() || ModuleName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETERS"), TEXT("Both 'emitter_name' and 'module_name' are required for get_module_info."), CanonicalSystemPath));
			}

			FNiagaraModuleInfo_Custom ModuleInfo;
			if (!UNiagaraEmitterService::GetModuleInfo(CanonicalSystemPath, EmitterName, ModuleName, ModuleInfo))
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MODULE_NOT_FOUND"), FString::Printf(TEXT("Module '%s' was not found on emitter '%s'."), *ModuleName, *EmitterName), CanonicalSystemPath));
			}

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Inspected module %s on emitter %s."), *ModuleInfo.ModuleName, *EmitterName),
				CanonicalSystemPath,
				NiagaraModuleInfoToJson(ModuleInfo),
				{},
				{},
				{},
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("add_module"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			const FString ModuleScriptPath = ExtractNiagaraParam(Params, TEXT("module_script_path"));
			const FString ModuleType = ExtractNiagaraParam(Params, TEXT("module_type"));
			if (EmitterName.IsEmpty() || ModuleScriptPath.IsEmpty() || ModuleType.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETERS"), TEXT("'emitter_name', 'module_script_path', and 'module_type' are required for add_module."), CanonicalSystemPath));
			}

			if (!UNiagaraEmitterService::AddModule(CanonicalSystemPath, EmitterName, ModuleScriptPath, ModuleType))
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("ADD_MODULE_FAILED"), FString::Printf(TEXT("Failed to add module '%s' to emitter '%s'."), *ModuleScriptPath, *EmitterName), CanonicalSystemPath));
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("added module %s to %s"), *ModuleScriptPath, *EmitterName));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Added module %s to emitter %s."), *ModuleScriptPath, *EmitterName),
				CanonicalSystemPath,
				nullptr,
				ChangedItems,
				{
					TEXT("Open the emitter stack and verify the module was inserted into the intended stage."),
					TEXT("Preview the system to confirm the module changes the behavior as expected.")
				},
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("remove_module"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			const FString ModuleName = ExtractNiagaraParam(Params, TEXT("module_name"));
			if (EmitterName.IsEmpty() || ModuleName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETERS"), TEXT("Both 'emitter_name' and 'module_name' are required for remove_module."), CanonicalSystemPath));
			}

			if (!UNiagaraEmitterService::RemoveModule(CanonicalSystemPath, EmitterName, ModuleName))
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("REMOVE_MODULE_FAILED"), FString::Printf(TEXT("Failed to remove module '%s' from emitter '%s'."), *ModuleName, *EmitterName), CanonicalSystemPath));
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("removed module %s from %s"), *ModuleName, *EmitterName));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Removed module %s from emitter %s."), *ModuleName, *EmitterName),
				CanonicalSystemPath,
				nullptr,
				ChangedItems,
				{
					TEXT("Open the emitter stack and verify the module is gone and stage ordering still makes sense."),
					TEXT("Preview the system to ensure removing the module did not break spawn/update behavior.")
				},
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("enable_module"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			const FString ModuleName = ExtractNiagaraParam(Params, TEXT("module_name"));
			const bool bEnabled = ExtractNiagaraBoolParam(Params, TEXT("enabled"), true);
			if (EmitterName.IsEmpty() || ModuleName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETERS"), TEXT("Both 'emitter_name' and 'module_name' are required for enable_module."), CanonicalSystemPath));
			}

			if (!UNiagaraEmitterService::EnableModule(CanonicalSystemPath, EmitterName, ModuleName, bEnabled))
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("ENABLE_MODULE_FAILED"), FString::Printf(TEXT("Failed to %s module '%s'."), bEnabled ? TEXT("enable") : TEXT("disable"), *ModuleName), CanonicalSystemPath));
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("%s module %s on %s"), bEnabled ? TEXT("enabled") : TEXT("disabled"), *ModuleName, *EmitterName));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("%s module %s on emitter %s."), bEnabled ? TEXT("Enabled") : TEXT("Disabled"), *ModuleName, *EmitterName),
				CanonicalSystemPath,
				nullptr,
				ChangedItems,
				{ TEXT("Open the emitter stack and verify the module enable toggle and preview output.") },
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("set_module_input"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			const FString ModuleName = ExtractNiagaraParam(Params, TEXT("module_name"));
			const FString InputName = ExtractNiagaraParam(Params, TEXT("input_name"));
			const FString Value = ExtractNiagaraParam(Params, TEXT("value"));
			if (EmitterName.IsEmpty() || ModuleName.IsEmpty() || InputName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETERS"), TEXT("'emitter_name', 'module_name', and 'input_name' are required for set_module_input."), CanonicalSystemPath));
			}

			if (!UNiagaraEmitterService::SetModuleInput(CanonicalSystemPath, EmitterName, ModuleName, InputName, Value))
			{
				return SerializeNiagaraJson(BuildNiagaraError(
					Action,
					TEXT("SET_MODULE_INPUT_FAILED"),
					FString::Printf(TEXT("Failed to set %s.%s to '%s'."), *ModuleName, *InputName, *Value),
					CanonicalSystemPath,
					{ TEXT("Use action=get_module_info or action=describe with emitter_name to inspect the exact module/input names.") }
				));
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("set module input %s.%s = %s"), *ModuleName, *InputName, *Value));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			const FString NewValue = UNiagaraEmitterService::GetModuleInput(CanonicalSystemPath, EmitterName, ModuleName, InputName);
			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetStringField(TEXT("emitter_name"), EmitterName);
			DataObj->SetStringField(TEXT("module_name"), ModuleName);
			DataObj->SetStringField(TEXT("input_name"), InputName);
			DataObj->SetStringField(TEXT("value"), NewValue);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Updated module input %s.%s on emitter %s."), *ModuleName, *InputName, *EmitterName),
				CanonicalSystemPath,
				DataObj,
				ChangedItems,
				{
					TEXT("Open the emitter stack and verify the module input value in the details panel."),
					TEXT("Preview the system to confirm the module input changes the effect as intended.")
				},
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("get_module_input"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			const FString ModuleName = ExtractNiagaraParam(Params, TEXT("module_name"));
			const FString InputName = ExtractNiagaraParam(Params, TEXT("input_name"));
			if (EmitterName.IsEmpty() || ModuleName.IsEmpty() || InputName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETERS"), TEXT("'emitter_name', 'module_name', and 'input_name' are required for get_module_input."), CanonicalSystemPath));
			}

			const FString Value = UNiagaraEmitterService::GetModuleInput(CanonicalSystemPath, EmitterName, ModuleName, InputName);
			if (Value.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MODULE_INPUT_NOT_FOUND"), FString::Printf(TEXT("Input '%s' was not found on module '%s'."), *InputName, *ModuleName), CanonicalSystemPath));
			}

			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetStringField(TEXT("emitter_name"), EmitterName);
			DataObj->SetStringField(TEXT("module_name"), ModuleName);
			DataObj->SetStringField(TEXT("input_name"), InputName);
			DataObj->SetStringField(TEXT("value"), Value);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Read module input %s.%s."), *ModuleName, *InputName),
				CanonicalSystemPath,
				DataObj,
				{},
				{},
				{},
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("set_module_di_user_parameter"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			const FString ModuleName = ExtractNiagaraParam(Params, TEXT("module_name"));
			const FString InputName = ExtractNiagaraParam(Params, TEXT("input_name"));
			const FString UserParameterName = ExtractNiagaraParam(Params, TEXT("value"));
			if (EmitterName.IsEmpty() || ModuleName.IsEmpty() || InputName.IsEmpty() || UserParameterName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETERS"), TEXT("'emitter_name', 'module_name', 'input_name', and 'value' are required for set_module_di_user_parameter."), CanonicalSystemPath));
			}

			if (!UNiagaraEmitterService::SetModuleDataInterfaceUserParameter(CanonicalSystemPath, EmitterName, ModuleName, InputName, UserParameterName))
			{
				return SerializeNiagaraJson(BuildNiagaraError(
					Action,
					TEXT("SET_MODULE_DI_USER_PARAMETER_FAILED"),
					FString::Printf(TEXT("Failed to bind module DI input %s.%s to '%s'."), *ModuleName, *InputName, *UserParameterName),
					CanonicalSystemPath,
					{ TEXT("Use action=get_module_info to inspect module inputs with input_source=DataInterface and confirm the exact input name.") }
				));
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("bound module DI input %s.%s -> %s"), *ModuleName, *InputName, *UserParameterName));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			FNiagaraModuleInfo_Custom ModuleInfo;
			const bool bHasInfo = UNiagaraEmitterService::GetModuleInfo(CanonicalSystemPath, EmitterName, ModuleName, ModuleInfo);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Bound module DI input %s.%s on emitter %s."), *ModuleName, *InputName, *EmitterName),
				CanonicalSystemPath,
				bHasInfo ? NiagaraModuleInfoToJson(ModuleInfo) : nullptr,
				ChangedItems,
				{
					TEXT("Open the emitter stack and confirm the Data Interface input now points at the intended user parameter."),
					TEXT("Preview the system and verify the module reads the bound runtime texture/input as expected.")
				},
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("list_renderers"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			if (EmitterName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_EMITTER_NAME"), TEXT("The 'emitter_name' parameter is required for list_renderers."), CanonicalSystemPath));
			}

			const TArray<FNiagaraRendererInfo_Custom> Renderers = UNiagaraEmitterService::ListRenderers(CanonicalSystemPath, EmitterName);
			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetStringField(TEXT("emitter_name"), EmitterName);
			DataObj->SetNumberField(TEXT("renderer_count"), Renderers.Num());
			DataObj->SetArrayField(TEXT("renderers"), NiagaraRendererArrayToJson(Renderers));

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Listed %d renderer(s) for emitter %s."), Renderers.Num(), *EmitterName),
				CanonicalSystemPath,
				DataObj,
				{},
				{},
				{},
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("get_renderer_details"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			const int32 RendererIndex = ExtractNiagaraIntParam(Params, TEXT("renderer_index"), INDEX_NONE, INDEX_NONE, 1024);
			if (EmitterName.IsEmpty() || RendererIndex == INDEX_NONE)
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETERS"), TEXT("Both 'emitter_name' and 'renderer_index' are required for get_renderer_details."), CanonicalSystemPath));
			}

			FNiagaraRendererDetailedInfo Details;
			if (!UNiagaraEmitterService::GetRendererDetails(CanonicalSystemPath, EmitterName, RendererIndex, Details))
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("RENDERER_NOT_FOUND"), FString::Printf(TEXT("Renderer index %d was not found on emitter '%s'."), RendererIndex, *EmitterName), CanonicalSystemPath));
			}

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Inspected renderer %d on emitter %s."), RendererIndex, *EmitterName),
				CanonicalSystemPath,
				NiagaraRendererDetailsToJson(Details),
				{},
				{},
				{},
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("add_renderer"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			const FString RendererType = ExtractNiagaraParam(Params, TEXT("renderer_type"));
			if (EmitterName.IsEmpty() || RendererType.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETERS"), TEXT("Both 'emitter_name' and 'renderer_type' are required for add_renderer."), CanonicalSystemPath));
			}

			if (!UNiagaraEmitterService::AddRenderer(CanonicalSystemPath, EmitterName, RendererType))
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("ADD_RENDERER_FAILED"), FString::Printf(TEXT("Failed to add renderer '%s' to emitter '%s'."), *RendererType, *EmitterName), CanonicalSystemPath));
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("added renderer %s to %s"), *RendererType, *EmitterName));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Added %s renderer to emitter %s."), *RendererType, *EmitterName),
				CanonicalSystemPath,
				nullptr,
				ChangedItems,
				{
					TEXT("Open the renderer stack and verify the new renderer type/material settings."),
					TEXT("Preview the system to confirm the renderer outputs visible particles as expected.")
				},
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("remove_renderer"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			const int32 RendererIndex = ExtractNiagaraIntParam(Params, TEXT("renderer_index"), INDEX_NONE, INDEX_NONE, 1024);
			if (EmitterName.IsEmpty() || RendererIndex == INDEX_NONE)
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETERS"), TEXT("Both 'emitter_name' and 'renderer_index' are required for remove_renderer."), CanonicalSystemPath));
			}

			if (!UNiagaraEmitterService::RemoveRenderer(CanonicalSystemPath, EmitterName, RendererIndex))
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("REMOVE_RENDERER_FAILED"), FString::Printf(TEXT("Failed to remove renderer index %d from emitter '%s'."), RendererIndex, *EmitterName), CanonicalSystemPath));
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("removed renderer index %d from %s"), RendererIndex, *EmitterName));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Removed renderer index %d from emitter %s."), RendererIndex, *EmitterName),
				CanonicalSystemPath,
				nullptr,
				ChangedItems,
				{ TEXT("Open the renderer stack and verify the remaining renderers/materials still match the intended output.") },
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("enable_renderer"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			const int32 RendererIndex = ExtractNiagaraIntParam(Params, TEXT("renderer_index"), INDEX_NONE, INDEX_NONE, 1024);
			const bool bEnabled = ExtractNiagaraBoolParam(Params, TEXT("enabled"), true);
			if (EmitterName.IsEmpty() || RendererIndex == INDEX_NONE)
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETERS"), TEXT("'emitter_name' and 'renderer_index' are required for enable_renderer."), CanonicalSystemPath));
			}

			if (!UNiagaraEmitterService::EnableRenderer(CanonicalSystemPath, EmitterName, RendererIndex, bEnabled))
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("ENABLE_RENDERER_FAILED"), FString::Printf(TEXT("Failed to %s renderer index %d on emitter '%s'."), bEnabled ? TEXT("enable") : TEXT("disable"), RendererIndex, *EmitterName), CanonicalSystemPath));
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("%s renderer index %d on %s"), bEnabled ? TEXT("enabled") : TEXT("disabled"), RendererIndex, *EmitterName));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("%s renderer %d on emitter %s."), bEnabled ? TEXT("Enabled") : TEXT("Disabled"), RendererIndex, *EmitterName),
				CanonicalSystemPath,
				nullptr,
				ChangedItems,
				{ TEXT("Open the renderer stack and verify the enable toggle and preview output.") },
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("set_renderer_property"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			const int32 RendererIndex = ExtractNiagaraIntParam(Params, TEXT("renderer_index"), INDEX_NONE, INDEX_NONE, 1024);
			const FString PropertyName = ExtractNiagaraParam(Params, TEXT("property_name"));
			const FString Value = ExtractNiagaraParam(Params, TEXT("value"));
			if (EmitterName.IsEmpty() || RendererIndex == INDEX_NONE || PropertyName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETERS"), TEXT("'emitter_name', 'renderer_index', and 'property_name' are required for set_renderer_property."), CanonicalSystemPath));
			}

			if (!UNiagaraEmitterService::SetRendererProperty(CanonicalSystemPath, EmitterName, RendererIndex, PropertyName, Value))
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("SET_RENDERER_PROPERTY_FAILED"), FString::Printf(TEXT("Failed to set renderer property '%s' to '%s'."), *PropertyName, *Value), CanonicalSystemPath));
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("set renderer[%d].%s = %s"), RendererIndex, *PropertyName, *Value));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Updated renderer property %s on emitter %s."), *PropertyName, *EmitterName),
				CanonicalSystemPath,
				nullptr,
				ChangedItems,
				{
					TEXT("Open the renderer details panel and verify the property value/material binding."),
					TEXT("Preview the system to confirm the renderer property affects the output as intended.")
				},
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("list_rapid_iteration_params"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			if (EmitterName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_EMITTER_NAME"), TEXT("The 'emitter_name' parameter is required for list_rapid_iteration_params."), CanonicalSystemPath));
			}

			const TArray<FNiagaraRIParameterInfo> Parameters = UNiagaraService::ListRapidIterationParams(CanonicalSystemPath, EmitterName);
			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetStringField(TEXT("emitter_name"), EmitterName);
			DataObj->SetNumberField(TEXT("parameter_count"), Parameters.Num());
			DataObj->SetArrayField(TEXT("parameters"), NiagaraRIParameterArrayToJson(Parameters));

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Listed %d rapid iteration parameter(s) for emitter %s."), Parameters.Num(), *EmitterName),
				CanonicalSystemPath,
				DataObj,
				{},
				{},
				{},
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("set_rapid_iteration_param"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			const FString ParameterName = ExtractNiagaraParam(Params, TEXT("parameter_name"));
			const FString Value = ExtractNiagaraParam(Params, TEXT("value"));
			if (EmitterName.IsEmpty() || ParameterName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETERS"), TEXT("Both 'emitter_name' and 'parameter_name' are required for set_rapid_iteration_param."), CanonicalSystemPath));
			}

			if (!UNiagaraService::SetRapidIterationParam(CanonicalSystemPath, EmitterName, ParameterName, Value))
			{
				return SerializeNiagaraJson(BuildNiagaraError(
					Action,
					TEXT("SET_RI_PARAM_FAILED"),
					FString::Printf(TEXT("Failed to set rapid iteration parameter '%s' to '%s'."), *ParameterName, *Value),
					CanonicalSystemPath,
					{ TEXT("Use action=list_rapid_iteration_params to inspect the exact parameter name and script stage variants.") }
				));
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("set RI param %s = %s"), *ParameterName, *Value));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Updated rapid iteration parameter %s on emitter %s."), *ParameterName, *EmitterName),
				CanonicalSystemPath,
				nullptr,
				ChangedItems,
				{
					TEXT("Open the emitter script stack and verify the parameter value on the affected stage(s)."),
					TEXT("Preview the system to confirm spawn/update behavior changed as expected.")
				},
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("set_rapid_iteration_param_by_stage"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			const FString ScriptType = ExtractNiagaraParam(Params, TEXT("script_type"));
			const FString ParameterName = ExtractNiagaraParam(Params, TEXT("parameter_name"));
			const FString Value = ExtractNiagaraParam(Params, TEXT("value"));
			if (EmitterName.IsEmpty() || ScriptType.IsEmpty() || ParameterName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_PARAMETERS"), TEXT("'emitter_name', 'script_type', and 'parameter_name' are required for set_rapid_iteration_param_by_stage."), CanonicalSystemPath));
			}

			if (!UNiagaraService::SetRapidIterationParamByStage(CanonicalSystemPath, EmitterName, ScriptType, ParameterName, Value))
			{
				return SerializeNiagaraJson(BuildNiagaraError(
					Action,
					TEXT("SET_RI_PARAM_BY_STAGE_FAILED"),
					FString::Printf(TEXT("Failed to set %s in %s to '%s'."), *ParameterName, *ScriptType, *Value),
					CanonicalSystemPath,
					{ TEXT("Use action=list_rapid_iteration_params to inspect exact script_type and parameter names.") }
				));
			}

			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("set %s in %s = %s"), *ParameterName, *ScriptType, *Value));
			TArray<FString> Warnings;
			ApplyPostNiagaraActions(NiagaraSystem, CanonicalSystemPath, Params, ChangedItems, Warnings);

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Updated rapid iteration parameter %s in %s."), *ParameterName, *ScriptType),
				CanonicalSystemPath,
				nullptr,
				ChangedItems,
				{
					TEXT("Open the targeted Niagara stage and verify the parameter changed only in the intended script."),
					TEXT("Preview the system to confirm the stage-specific behavior change.")
				},
				Warnings,
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("get_emitter_lifecycle"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			if (EmitterName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_EMITTER_NAME"), TEXT("The 'emitter_name' parameter is required for get_emitter_lifecycle."), CanonicalSystemPath));
			}

			FNiagaraEmitterLifecycleInfo LifecycleInfo;
			if (!UNiagaraService::GetEmitterLifecycle(CanonicalSystemPath, EmitterName, LifecycleInfo))
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("EMITTER_LIFECYCLE_NOT_FOUND"), FString::Printf(TEXT("Failed to inspect lifecycle for emitter '%s'."), *EmitterName), CanonicalSystemPath));
			}

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Inspected lifecycle for emitter %s."), *EmitterName),
				CanonicalSystemPath,
				NiagaraEmitterLifecycleToJson(LifecycleInfo),
				{},
				{},
				{},
				{ CanonicalSystemPath }
			));
		}

		if (Action == TEXT("get_emitter_properties"))
		{
			const FString EmitterName = ExtractNiagaraParam(Params, TEXT("emitter_name"));
			if (EmitterName.IsEmpty())
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("MISSING_EMITTER_NAME"), TEXT("The 'emitter_name' parameter is required for get_emitter_properties."), CanonicalSystemPath));
			}

			FNiagaraEmitterPropertiesInfo EmitterProperties;
			if (!UNiagaraEmitterService::GetEmitterProperties(CanonicalSystemPath, EmitterName, EmitterProperties))
			{
				return SerializeNiagaraJson(BuildNiagaraError(Action, TEXT("EMITTER_PROPERTIES_NOT_FOUND"), FString::Printf(TEXT("Failed to inspect properties for emitter '%s'."), *EmitterName), CanonicalSystemPath));
			}

			return SerializeNiagaraJson(BuildNiagaraSuccess(
				Action,
				FString::Printf(TEXT("Inspected properties for emitter %s."), *EmitterName),
				CanonicalSystemPath,
				NiagaraEmitterPropertiesToJson(EmitterProperties),
				{},
				{},
				{},
				{ CanonicalSystemPath }
			));
		}

		return SerializeNiagaraJson(BuildNiagaraError(
			Action,
			TEXT("UNKNOWN_ACTION"),
			FString::Printf(TEXT("Unknown niagara_system action '%s'. Use action=help for documentation."), *Action)
		));
	}
);
