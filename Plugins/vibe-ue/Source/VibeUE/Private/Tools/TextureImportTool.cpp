// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Core/ToolRegistry.h"
#include "Core/ErrorCodes.h"
#include "PythonAPI/UAssetDiscoveryService.h"

#include "AssetRegistry/AssetData.h"
#include "Containers/Ticker.h"
#include "EditorAssetLibrary.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureDefines.h"
#include "HAL/CriticalSection.h"
#include "Json.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

namespace
{
	enum class ETextureImportJobState : uint8
	{
		Queued,
		Running,
		Succeeded,
		Failed
	};

	struct FTextureImportSettings
	{
		FString ProfileName = TEXT("default");
		bool bHasCompressionSettings = false;
		TextureCompressionSettings CompressionSettings = TC_Default;
		bool bHasFilter = false;
		TextureFilter Filter = TF_Default;
		bool bHasMipGenSettings = false;
		TextureMipGenSettings MipGenSettings = TMGS_FromTextureGroup;
		bool bHasLODGroup = false;
		TextureGroup LODGroup = TEXTUREGROUP_World;
		bool bHasSRGB = false;
		bool bSRGB = false;
		bool bHasCompressionNoAlpha = false;
		bool bCompressionNoAlpha = false;
		bool bHasCompressionNone = false;
		bool bCompressionNone = false;
		bool bHasNeverStream = false;
		bool bNeverStream = false;
		bool bHasVirtualTextureStreaming = false;
		bool bVirtualTextureStreaming = false;
		bool bHasAddressX = false;
		TextureAddress AddressX = TA_Wrap;
		bool bHasAddressY = false;
		TextureAddress AddressY = TA_Wrap;
	};

	struct FTextureImportJob
	{
		FString ImportId;
		FString SourceFilePath;
		FString DestinationPath;
		bool bReplaceExisting = false;
		FTextureImportSettings Settings;
		ETextureImportJobState State = ETextureImportJobState::Queued;
		FString ErrorCode;
		FString ErrorMessage;
		FString ImportedAssetPath;
		FDateTime SubmittedAtUtc;
		FDateTime StartedAtUtc;
		FDateTime CompletedAtUtc;
	};

	static FCriticalSection GTextureImportJobLock;
	static TMap<FString, FTextureImportJob> GTextureImportJobs;
	static TArray<FString> GTextureImportQueue;
	static bool GbTextureImportBusy = false;
	static FTSTicker::FDelegateHandle GTextureImportTickerHandle;

	static FString NormalizeTextureImportToken(const FString& Value)
	{
		FString Normalized;
		Normalized.Reserve(Value.Len());
		for (const TCHAR Character : Value)
		{
			if (FChar::IsAlnum(Character))
			{
				Normalized.AppendChar(FChar::ToLower(Character));
			}
		}
		return Normalized;
	}

	static bool TryExtractTextureImportParam(const TMap<FString, FString>& Params, const FString& FieldName, FString& OutValue)
	{
		if (const FString* DirectParam = Params.Find(FieldName))
		{
			OutValue = *DirectParam;
			return true;
		}

		FString CapitalizedField = FieldName;
		if (!CapitalizedField.IsEmpty())
		{
			CapitalizedField[0] = FChar::ToUpper(CapitalizedField[0]);
		}

		if (const FString* CapitalizedParam = Params.Find(CapitalizedField))
		{
			OutValue = *CapitalizedParam;
			return true;
		}

		const FString* ParamsJsonString = Params.Find(TEXT("ParamsJson"));
		if (!ParamsJsonString)
		{
			return false;
		}

		TSharedPtr<FJsonObject> JsonObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ParamsJsonString);
		if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
		{
			return false;
		}

		if (JsonObject->TryGetStringField(FieldName, OutValue))
		{
			return true;
		}

		return false;
	}

	static FString ExtractTextureImportParam(const TMap<FString, FString>& Params, const FString& FieldName)
	{
		FString Value;
		return TryExtractTextureImportParam(Params, FieldName, Value) ? Value : FString();
	}

	static bool ExtractTextureImportBool(const TMap<FString, FString>& Params, const FString& FieldName, bool bDefaultValue)
	{
		FString Value;
		if (!TryExtractTextureImportParam(Params, FieldName, Value))
		{
			return bDefaultValue;
		}

		return Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("1"));
	}

	template<typename TEnum>
	static bool TryParseTextureImportEnum(const FString& RawValue, const TMap<FString, TEnum>& Aliases, TEnum& OutValue)
	{
		const FString NormalizedValue = NormalizeTextureImportToken(RawValue);
		if (NormalizedValue.IsEmpty())
		{
			return false;
		}

		if (const TEnum* AliasValue = Aliases.Find(NormalizedValue))
		{
			OutValue = *AliasValue;
			return true;
		}

		const UEnum* EnumType = StaticEnum<TEnum>();
		if (!EnumType)
		{
			return false;
		}

		for (int32 Index = 0; Index < EnumType->NumEnums(); ++Index)
		{
			const int64 EnumValue = EnumType->GetValueByIndex(Index);
			if (EnumValue < 0)
			{
				continue;
			}

			const FString NameToken = NormalizeTextureImportToken(EnumType->GetNameStringByValue(EnumValue));
			const FString DisplayToken = NormalizeTextureImportToken(EnumType->GetDisplayNameTextByValue(EnumValue).ToString());
			if (NormalizedValue == NameToken || (!DisplayToken.IsEmpty() && NormalizedValue == DisplayToken))
			{
				OutValue = static_cast<TEnum>(EnumValue);
				return true;
			}
		}

		return false;
	}

	template<typename TEnum>
	static FString GetTextureImportEnumName(const TEnum Value)
	{
		const UEnum* EnumType = StaticEnum<TEnum>();
		return EnumType ? EnumType->GetNameStringByValue(static_cast<int64>(Value)) : FString();
	}

	static bool TryParseTextureFilter(const FString& RawValue, TextureFilter& OutValue)
	{
		static const TMap<FString, TextureFilter> Aliases = {
			{ TEXT("default"), TF_Default },
			{ TEXT("groupdefault"), TF_Default },
			{ TEXT("nearest"), TF_Nearest },
			{ TEXT("point"), TF_Nearest },
			{ TEXT("bilinear"), TF_Bilinear },
			{ TEXT("linear"), TF_Bilinear },
			{ TEXT("trilinear"), TF_Trilinear }
		};
		return TryParseTextureImportEnum<TextureFilter>(RawValue, Aliases, OutValue);
	}

	static bool TryParseTextureCompressionSettings(const FString& RawValue, TextureCompressionSettings& OutValue)
	{
		static const TMap<FString, TextureCompressionSettings> Aliases = {
			{ TEXT("default"), TC_Default },
			{ TEXT("ui"), TC_EditorIcon },
			{ TEXT("userinterface"), TC_EditorIcon },
			{ TEXT("userinterface2d"), TC_EditorIcon },
			{ TEXT("editoricon"), TC_EditorIcon },
			{ TEXT("mask"), TC_Masks },
			{ TEXT("masks"), TC_Masks },
			{ TEXT("normal"), TC_Normalmap },
			{ TEXT("normalmap"), TC_Normalmap },
			{ TEXT("grayscale"), TC_Grayscale },
			{ TEXT("alpha"), TC_Alpha },
			{ TEXT("rgba"), TC_VectorDisplacementmap },
			{ TEXT("vectorrgba"), TC_VectorDisplacementmap },
			{ TEXT("vectordisplacementmap"), TC_VectorDisplacementmap },
			{ TEXT("bc7"), TC_BC7 },
			{ TEXT("hdr"), TC_HDR }
		};
		return TryParseTextureImportEnum<TextureCompressionSettings>(RawValue, Aliases, OutValue);
	}

	static bool TryParseTextureMipGenSettings(const FString& RawValue, TextureMipGenSettings& OutValue)
	{
		static const TMap<FString, TextureMipGenSettings> Aliases = {
			{ TEXT("default"), TMGS_FromTextureGroup },
			{ TEXT("fromtexturegroup"), TMGS_FromTextureGroup },
			{ TEXT("fromgroup"), TMGS_FromTextureGroup },
			{ TEXT("nomipmaps"), TMGS_NoMipmaps },
			{ TEXT("nomips"), TMGS_NoMipmaps },
			{ TEXT("none"), TMGS_NoMipmaps },
			{ TEXT("simpleaverage"), TMGS_SimpleAverage },
			{ TEXT("simple"), TMGS_SimpleAverage },
			{ TEXT("sharpen0"), TMGS_Sharpen0 },
			{ TEXT("leaveexistingmips"), TMGS_LeaveExistingMips }
		};
		return TryParseTextureImportEnum<TextureMipGenSettings>(RawValue, Aliases, OutValue);
	}

	static bool TryParseTextureGroupValue(const FString& RawValue, TextureGroup& OutValue)
	{
		static const TMap<FString, TextureGroup> Aliases = {
			{ TEXT("world"), TEXTUREGROUP_World },
			{ TEXT("worldnormalmap"), TEXTUREGROUP_WorldNormalMap },
			{ TEXT("ui"), TEXTUREGROUP_UI },
			{ TEXT("pixels2d"), TEXTUREGROUP_Pixels2D },
			{ TEXT("2dpixels"), TEXTUREGROUP_Pixels2D },
			{ TEXT("pixelart"), TEXTUREGROUP_Pixels2D }
		};
		return TryParseTextureImportEnum<TextureGroup>(RawValue, Aliases, OutValue);
	}

	static bool TryParseTextureAddressValue(const FString& RawValue, TextureAddress& OutValue)
	{
		static const TMap<FString, TextureAddress> Aliases = {
			{ TEXT("wrap"), TA_Wrap },
			{ TEXT("repeat"), TA_Wrap },
			{ TEXT("clamp"), TA_Clamp },
			{ TEXT("mirror"), TA_Mirror }
		};
		return TryParseTextureImportEnum<TextureAddress>(RawValue, Aliases, OutValue);
	}

	static bool HasAnyResolvedTextureImportSettings(const FTextureImportSettings& Settings)
	{
		return Settings.bHasCompressionSettings
			|| Settings.bHasFilter
			|| Settings.bHasMipGenSettings
			|| Settings.bHasLODGroup
			|| Settings.bHasSRGB
			|| Settings.bHasCompressionNoAlpha
			|| Settings.bHasCompressionNone
			|| Settings.bHasNeverStream
			|| Settings.bHasVirtualTextureStreaming
			|| Settings.bHasAddressX
			|| Settings.bHasAddressY;
	}

	static void ApplyTextureImportProfileDefaults(const FString& RequestedProfile, FTextureImportSettings& Settings, bool& bOutRecognized)
	{
		const FString ProfileToken = NormalizeTextureImportToken(RequestedProfile);
		if (ProfileToken.IsEmpty() || ProfileToken == TEXT("default"))
		{
			Settings.ProfileName = TEXT("default");
			bOutRecognized = true;
			return;
		}

		if (ProfileToken == TEXT("ui"))
		{
			Settings.ProfileName = TEXT("ui");
			Settings.bHasCompressionSettings = true;
			Settings.CompressionSettings = TC_EditorIcon;
			Settings.bHasFilter = true;
			Settings.Filter = TF_Trilinear;
			Settings.bHasMipGenSettings = true;
			Settings.MipGenSettings = TMGS_NoMipmaps;
			Settings.bHasLODGroup = true;
			Settings.LODGroup = TEXTUREGROUP_UI;
			Settings.bHasSRGB = true;
			Settings.bSRGB = true;
			Settings.bHasNeverStream = true;
			Settings.bNeverStream = true;
			Settings.bHasVirtualTextureStreaming = true;
			Settings.bVirtualTextureStreaming = false;
			Settings.bHasAddressX = true;
			Settings.AddressX = TA_Clamp;
			Settings.bHasAddressY = true;
			Settings.AddressY = TA_Clamp;
			bOutRecognized = true;
			return;
		}

		if (ProfileToken == TEXT("uipixel") || ProfileToken == TEXT("pixelui") || ProfileToken == TEXT("uinearest"))
		{
			Settings.ProfileName = TEXT("ui_pixel");
			Settings.bHasCompressionSettings = true;
			Settings.CompressionSettings = TC_EditorIcon;
			Settings.bHasFilter = true;
			Settings.Filter = TF_Nearest;
			Settings.bHasMipGenSettings = true;
			Settings.MipGenSettings = TMGS_NoMipmaps;
			Settings.bHasLODGroup = true;
			Settings.LODGroup = TEXTUREGROUP_Pixels2D;
			Settings.bHasSRGB = true;
			Settings.bSRGB = true;
			Settings.bHasNeverStream = true;
			Settings.bNeverStream = true;
			Settings.bHasVirtualTextureStreaming = true;
			Settings.bVirtualTextureStreaming = false;
			Settings.bHasAddressX = true;
			Settings.AddressX = TA_Clamp;
			Settings.bHasAddressY = true;
			Settings.AddressY = TA_Clamp;
			bOutRecognized = true;
			return;
		}

		if (ProfileToken == TEXT("uimask") || ProfileToken == TEXT("maskui"))
		{
			Settings.ProfileName = TEXT("ui_mask");
			Settings.bHasCompressionSettings = true;
			Settings.CompressionSettings = TC_Masks;
			Settings.bHasFilter = true;
			Settings.Filter = TF_Bilinear;
			Settings.bHasMipGenSettings = true;
			Settings.MipGenSettings = TMGS_NoMipmaps;
			Settings.bHasLODGroup = true;
			Settings.LODGroup = TEXTUREGROUP_UI;
			Settings.bHasSRGB = true;
			Settings.bSRGB = false;
			Settings.bHasCompressionNoAlpha = true;
			Settings.bCompressionNoAlpha = false;
			Settings.bHasNeverStream = true;
			Settings.bNeverStream = true;
			Settings.bHasVirtualTextureStreaming = true;
			Settings.bVirtualTextureStreaming = false;
			Settings.bHasAddressX = true;
			Settings.AddressX = TA_Clamp;
			Settings.bHasAddressY = true;
			Settings.AddressY = TA_Clamp;
			bOutRecognized = true;
			return;
		}

		if (ProfileToken == TEXT("rgbadata") || ProfileToken == TEXT("datargba") || ProfileToken == TEXT("data"))
		{
			Settings.ProfileName = TEXT("rgba_data");
			Settings.bHasCompressionSettings = true;
			Settings.CompressionSettings = TC_VectorDisplacementmap;
			Settings.bHasFilter = true;
			Settings.Filter = TF_Nearest;
			Settings.bHasMipGenSettings = true;
			Settings.MipGenSettings = TMGS_NoMipmaps;
			Settings.bHasSRGB = true;
			Settings.bSRGB = false;
			Settings.bHasCompressionNoAlpha = true;
			Settings.bCompressionNoAlpha = false;
			Settings.bHasCompressionNone = true;
			Settings.bCompressionNone = false;
			Settings.bHasNeverStream = true;
			Settings.bNeverStream = true;
			Settings.bHasVirtualTextureStreaming = true;
			Settings.bVirtualTextureStreaming = false;
			Settings.bHasAddressX = true;
			Settings.AddressX = TA_Clamp;
			Settings.bHasAddressY = true;
			Settings.AddressY = TA_Clamp;
			bOutRecognized = true;
			return;
		}

		if (ProfileToken == TEXT("normal") || ProfileToken == TEXT("normalmap"))
		{
			Settings.ProfileName = TEXT("normal_map");
			Settings.bHasCompressionSettings = true;
			Settings.CompressionSettings = TC_Normalmap;
			Settings.bHasMipGenSettings = true;
			Settings.MipGenSettings = TMGS_FromTextureGroup;
			Settings.bHasLODGroup = true;
			Settings.LODGroup = TEXTUREGROUP_WorldNormalMap;
			Settings.bHasSRGB = true;
			Settings.bSRGB = false;
			Settings.bHasCompressionNoAlpha = true;
			Settings.bCompressionNoAlpha = true;
			Settings.bHasNeverStream = true;
			Settings.bNeverStream = false;
			Settings.bHasVirtualTextureStreaming = true;
			Settings.bVirtualTextureStreaming = false;
			bOutRecognized = true;
			return;
		}

		bOutRecognized = false;
	}

	static bool ResolveTextureImportSettings(const TMap<FString, FString>& Params, FTextureImportSettings& OutSettings, FString& OutErrorCode, FString& OutErrorMessage)
	{
		OutSettings = FTextureImportSettings();

		const FString RequestedProfile = ExtractTextureImportParam(Params, TEXT("profile"));
		bool bProfileRecognized = false;
		ApplyTextureImportProfileDefaults(RequestedProfile, OutSettings, bProfileRecognized);
		if (!bProfileRecognized)
		{
			OutErrorCode = VibeUE::ErrorCodes::PARAM_INVALID;
			OutErrorMessage = FString::Printf(
				TEXT("Unknown texture import profile '%s'. Supported profiles: default, ui, ui_pixel, ui_mask, rgba_data, normal_map."),
				*RequestedProfile
			);
			return false;
		}

		FString RawValue;
		bool bBoolValue = false;

		if (TryExtractTextureImportParam(Params, TEXT("filter"), RawValue))
		{
			TextureFilter ParsedValue = TF_Default;
			if (!TryParseTextureFilter(RawValue, ParsedValue))
			{
				OutErrorCode = VibeUE::ErrorCodes::PARAM_INVALID;
				OutErrorMessage = FString::Printf(TEXT("Invalid filter value '%s'."), *RawValue);
				return false;
			}
			OutSettings.bHasFilter = true;
			OutSettings.Filter = ParsedValue;
		}

		if (TryExtractTextureImportParam(Params, TEXT("compression_settings"), RawValue))
		{
			TextureCompressionSettings ParsedValue = TC_Default;
			if (!TryParseTextureCompressionSettings(RawValue, ParsedValue))
			{
				OutErrorCode = VibeUE::ErrorCodes::PARAM_INVALID;
				OutErrorMessage = FString::Printf(TEXT("Invalid compression_settings value '%s'."), *RawValue);
				return false;
			}
			OutSettings.bHasCompressionSettings = true;
			OutSettings.CompressionSettings = ParsedValue;
		}

		if (TryExtractTextureImportParam(Params, TEXT("mip_gen_settings"), RawValue))
		{
			TextureMipGenSettings ParsedValue = TMGS_FromTextureGroup;
			if (!TryParseTextureMipGenSettings(RawValue, ParsedValue))
			{
				OutErrorCode = VibeUE::ErrorCodes::PARAM_INVALID;
				OutErrorMessage = FString::Printf(TEXT("Invalid mip_gen_settings value '%s'."), *RawValue);
				return false;
			}
			OutSettings.bHasMipGenSettings = true;
			OutSettings.MipGenSettings = ParsedValue;
		}

		if (TryExtractTextureImportParam(Params, TEXT("lod_group"), RawValue))
		{
			TextureGroup ParsedValue = TEXTUREGROUP_World;
			if (!TryParseTextureGroupValue(RawValue, ParsedValue))
			{
				OutErrorCode = VibeUE::ErrorCodes::PARAM_INVALID;
				OutErrorMessage = FString::Printf(TEXT("Invalid lod_group value '%s'."), *RawValue);
				return false;
			}
			OutSettings.bHasLODGroup = true;
			OutSettings.LODGroup = ParsedValue;
		}

		if (TryExtractTextureImportParam(Params, TEXT("address_x"), RawValue))
		{
			TextureAddress ParsedValue = TA_Wrap;
			if (!TryParseTextureAddressValue(RawValue, ParsedValue))
			{
				OutErrorCode = VibeUE::ErrorCodes::PARAM_INVALID;
				OutErrorMessage = FString::Printf(TEXT("Invalid address_x value '%s'."), *RawValue);
				return false;
			}
			OutSettings.bHasAddressX = true;
			OutSettings.AddressX = ParsedValue;
		}

		if (TryExtractTextureImportParam(Params, TEXT("address_y"), RawValue))
		{
			TextureAddress ParsedValue = TA_Wrap;
			if (!TryParseTextureAddressValue(RawValue, ParsedValue))
			{
				OutErrorCode = VibeUE::ErrorCodes::PARAM_INVALID;
				OutErrorMessage = FString::Printf(TEXT("Invalid address_y value '%s'."), *RawValue);
				return false;
			}
			OutSettings.bHasAddressY = true;
			OutSettings.AddressY = ParsedValue;
		}

		if (TryExtractTextureImportParam(Params, TEXT("srgb"), RawValue))
		{
			bBoolValue = RawValue.Equals(TEXT("true"), ESearchCase::IgnoreCase) || RawValue.Equals(TEXT("1"));
			OutSettings.bHasSRGB = true;
			OutSettings.bSRGB = bBoolValue;
		}

		if (TryExtractTextureImportParam(Params, TEXT("compression_no_alpha"), RawValue))
		{
			bBoolValue = RawValue.Equals(TEXT("true"), ESearchCase::IgnoreCase) || RawValue.Equals(TEXT("1"));
			OutSettings.bHasCompressionNoAlpha = true;
			OutSettings.bCompressionNoAlpha = bBoolValue;
		}

		if (TryExtractTextureImportParam(Params, TEXT("compression_none"), RawValue))
		{
			bBoolValue = RawValue.Equals(TEXT("true"), ESearchCase::IgnoreCase) || RawValue.Equals(TEXT("1"));
			OutSettings.bHasCompressionNone = true;
			OutSettings.bCompressionNone = bBoolValue;
		}

		if (TryExtractTextureImportParam(Params, TEXT("never_stream"), RawValue))
		{
			bBoolValue = RawValue.Equals(TEXT("true"), ESearchCase::IgnoreCase) || RawValue.Equals(TEXT("1"));
			OutSettings.bHasNeverStream = true;
			OutSettings.bNeverStream = bBoolValue;
		}

		if (TryExtractTextureImportParam(Params, TEXT("virtual_texture_streaming"), RawValue))
		{
			bBoolValue = RawValue.Equals(TEXT("true"), ESearchCase::IgnoreCase) || RawValue.Equals(TEXT("1"));
			OutSettings.bHasVirtualTextureStreaming = true;
			OutSettings.bVirtualTextureStreaming = bBoolValue;
		}

		return true;
	}

	static FString SerializeTextureImportJson(const TSharedPtr<FJsonObject>& JsonObject)
	{
		FString JsonString;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
		FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
		return JsonString;
	}

	static FString JobStateToString(const ETextureImportJobState State)
	{
		switch (State)
		{
		case ETextureImportJobState::Queued:
			return TEXT("queued");
		case ETextureImportJobState::Running:
			return TEXT("running");
		case ETextureImportJobState::Succeeded:
			return TEXT("succeeded");
		case ETextureImportJobState::Failed:
			return TEXT("failed");
		default:
			return TEXT("unknown");
		}
	}

	static FString ToIso8601Utc(const FDateTime& Value)
	{
		return Value.GetTicks() > 0 ? Value.ToIso8601() : FString();
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

	static TSharedPtr<FJsonObject> BuildResolvedTextureImportSettingsJson(const FTextureImportSettings& Settings)
	{
		const TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
		RootObj->SetStringField(TEXT("profile"), Settings.ProfileName);
		if (Settings.bHasCompressionSettings)
		{
			RootObj->SetStringField(TEXT("compression_settings"), GetTextureImportEnumName(Settings.CompressionSettings));
		}
		if (Settings.bHasFilter)
		{
			RootObj->SetStringField(TEXT("filter"), GetTextureImportEnumName(Settings.Filter));
		}
		if (Settings.bHasMipGenSettings)
		{
			RootObj->SetStringField(TEXT("mip_gen_settings"), GetTextureImportEnumName(Settings.MipGenSettings));
		}
		if (Settings.bHasLODGroup)
		{
			RootObj->SetStringField(TEXT("lod_group"), GetTextureImportEnumName(Settings.LODGroup));
		}
		if (Settings.bHasSRGB)
		{
			RootObj->SetBoolField(TEXT("srgb"), Settings.bSRGB);
		}
		if (Settings.bHasCompressionNoAlpha)
		{
			RootObj->SetBoolField(TEXT("compression_no_alpha"), Settings.bCompressionNoAlpha);
		}
		if (Settings.bHasCompressionNone)
		{
			RootObj->SetBoolField(TEXT("compression_none"), Settings.bCompressionNone);
		}
		if (Settings.bHasNeverStream)
		{
			RootObj->SetBoolField(TEXT("never_stream"), Settings.bNeverStream);
		}
		if (Settings.bHasVirtualTextureStreaming)
		{
			RootObj->SetBoolField(TEXT("virtual_texture_streaming"), Settings.bVirtualTextureStreaming);
		}
		if (Settings.bHasAddressX)
		{
			RootObj->SetStringField(TEXT("address_x"), GetTextureImportEnumName(Settings.AddressX));
		}
		if (Settings.bHasAddressY)
		{
			RootObj->SetStringField(TEXT("address_y"), GetTextureImportEnumName(Settings.AddressY));
		}
		return RootObj;
	}

	static TSharedPtr<FJsonObject> BuildTextureImportError(const FString& ErrorCode, const FString& ErrorMessage, const FString& ImportId = TEXT(""))
	{
		const TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
		RootObj->SetBoolField(TEXT("success"), false);
		RootObj->SetStringField(TEXT("error_code"), ErrorCode);
		RootObj->SetStringField(TEXT("error_message"), ErrorMessage);
		if (!ImportId.IsEmpty())
		{
			RootObj->SetStringField(TEXT("import_id"), ImportId);
		}
		return RootObj;
	}

	static TSharedPtr<FJsonObject> BuildTextureImportJobResponse(const FString& Action, const FTextureImportJob& Job)
	{
		const bool bSuccess = Job.State != ETextureImportJobState::Failed;
		const TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
		RootObj->SetBoolField(TEXT("success"), bSuccess);
		RootObj->SetStringField(TEXT("action"), Action);
		RootObj->SetStringField(TEXT("import_id"), Job.ImportId);
		RootObj->SetStringField(TEXT("status"), JobStateToString(Job.State));
		RootObj->SetStringField(TEXT("source_file_path"), Job.SourceFilePath);
		RootObj->SetStringField(TEXT("destination_path"), Job.DestinationPath);
		RootObj->SetBoolField(TEXT("replace_existing"), Job.bReplaceExisting);
		RootObj->SetObjectField(TEXT("resolved_settings"), BuildResolvedTextureImportSettingsJson(Job.Settings));
		RootObj->SetStringField(TEXT("submitted_at_utc"), ToIso8601Utc(Job.SubmittedAtUtc));
		if (Job.StartedAtUtc.GetTicks() > 0)
		{
			RootObj->SetStringField(TEXT("started_at_utc"), ToIso8601Utc(Job.StartedAtUtc));
		}
		if (Job.CompletedAtUtc.GetTicks() > 0)
		{
			RootObj->SetStringField(TEXT("completed_at_utc"), ToIso8601Utc(Job.CompletedAtUtc));
		}
		if (!Job.ImportedAssetPath.IsEmpty())
		{
			RootObj->SetStringField(TEXT("asset_path"), Job.ImportedAssetPath);
		}

		switch (Job.State)
		{
		case ETextureImportJobState::Queued:
			RootObj->SetStringField(TEXT("summary"), FString::Printf(TEXT("Texture import has been queued with profile '%s'."), *Job.Settings.ProfileName));
			RootObj->SetNumberField(TEXT("poll_after_ms"), 250);
			AddStringArrayField(RootObj, TEXT("verification_hints"), {
				TEXT("Wait for the next status poll before assuming the asset exists."),
				TEXT("Do not submit the same file repeatedly while a queued import is still pending."),
				TEXT("Use profile plus explicit overrides for UI, mask, normal-map, or RGBA data imports.")
			});
			break;
		case ETextureImportJobState::Running:
			RootObj->SetStringField(TEXT("summary"), TEXT("Texture import is currently running inside the editor."));
			RootObj->SetNumberField(TEXT("poll_after_ms"), 250);
			AddStringArrayField(RootObj, TEXT("verification_hints"), {
				TEXT("Keep polling with the import_id until the import finishes."),
				TEXT("Avoid closing the editor while the import is running.")
			});
			break;
		case ETextureImportJobState::Succeeded:
			RootObj->SetStringField(TEXT("summary"), FString::Printf(TEXT("Texture import completed successfully. Profile '%s' and any explicit overrides were applied."), *Job.Settings.ProfileName));
			AddStringArrayField(RootObj, TEXT("verification_hints"), {
				TEXT("Open the imported texture asset to verify compression, sampling filter, mip generation, and sRGB."),
				TEXT("If this is a UI texture, verify the imported asset path before wiring it into UMG or materials.")
			});
			break;
		case ETextureImportJobState::Failed:
			RootObj->SetStringField(TEXT("summary"), TEXT("Texture import failed."));
			RootObj->SetStringField(TEXT("error_code"), Job.ErrorCode);
			RootObj->SetStringField(TEXT("error_message"), Job.ErrorMessage);
			AddStringArrayField(RootObj, TEXT("verification_hints"), {
				TEXT("Inspect the source file path, destination path, and requested texture settings, then retry with a new import request."),
				TEXT("If the destination asset already exists, either delete it manually or retry with replace_existing=true.")
			});
			break;
		}

		return RootObj;
	}

	static bool IsSupportedTextureSource(const FString& SourceFilePath)
	{
		const FString Extension = FPaths::GetExtension(SourceFilePath).ToLower();
		static const TSet<FString> SupportedExtensions = {
			TEXT("png"),
			TEXT("jpg"),
			TEXT("jpeg"),
			TEXT("bmp"),
			TEXT("tga"),
			TEXT("psd"),
			TEXT("exr"),
			TEXT("hdr")
		};
		return SupportedExtensions.Contains(Extension);
	}

	static bool IsValidDestinationAssetPath(const FString& DestinationPath)
	{
		return DestinationPath.StartsWith(TEXT("/Game/")) && DestinationPath.Len() > FString(TEXT("/Game/")).Len();
	}

	static FString GetPackagePathFromAssetPath(const FString& AssetPath)
	{
		FString PackagePath;
		FString AssetName;
		if (AssetPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
		{
			return PackagePath;
		}
		return TEXT("/Game");
	}

	static void TrimTextureImportHistory_NoLock()
	{
		if (GTextureImportJobs.Num() <= 64)
		{
			return;
		}

		TArray<FString> RemovableIds;
		for (const TPair<FString, FTextureImportJob>& Pair : GTextureImportJobs)
		{
			if (Pair.Value.State == ETextureImportJobState::Succeeded || Pair.Value.State == ETextureImportJobState::Failed)
			{
				RemovableIds.Add(Pair.Key);
			}
		}

		RemovableIds.Sort([&](const FString& A, const FString& B)
		{
			const FTextureImportJob* JobA = GTextureImportJobs.Find(A);
			const FTextureImportJob* JobB = GTextureImportJobs.Find(B);
			const FDateTime TimeA = JobA ? JobA->CompletedAtUtc : FDateTime::MinValue();
			const FDateTime TimeB = JobB ? JobB->CompletedAtUtc : FDateTime::MinValue();
			return TimeA < TimeB;
		});

		for (const FString& ImportId : RemovableIds)
		{
			if (GTextureImportJobs.Num() <= 64)
			{
				break;
			}
			GTextureImportJobs.Remove(ImportId);
		}
	}

	static bool ApplyResolvedTextureImportSettings(UObject* ImportedAsset, const FString& AssetPath, const FTextureImportSettings& Settings, FString& OutErrorMessage)
	{
		if (!HasAnyResolvedTextureImportSettings(Settings))
		{
			return true;
		}

		UTexture* Texture = Cast<UTexture>(ImportedAsset);
		if (!Texture)
		{
			OutErrorMessage = FString::Printf(TEXT("Imported asset is not a texture: %s"), *AssetPath);
			return false;
		}

		Texture->Modify();

		if (Settings.bHasCompressionSettings)
		{
			Texture->CompressionSettings = Settings.CompressionSettings;
		}
		if (Settings.bHasFilter)
		{
			Texture->Filter = Settings.Filter;
		}
		if (Settings.bHasMipGenSettings)
		{
			Texture->MipGenSettings = Settings.MipGenSettings;
		}
		if (Settings.bHasLODGroup)
		{
			Texture->LODGroup = Settings.LODGroup;
		}
		if (Settings.bHasSRGB)
		{
			Texture->SRGB = Settings.bSRGB;
		}
		if (Settings.bHasCompressionNoAlpha)
		{
			Texture->CompressionNoAlpha = Settings.bCompressionNoAlpha;
		}
		if (Settings.bHasCompressionNone)
		{
			Texture->CompressionNone = Settings.bCompressionNone;
		}
		if (Settings.bHasNeverStream)
		{
			Texture->NeverStream = Settings.bNeverStream;
		}
		if (Settings.bHasVirtualTextureStreaming)
		{
			Texture->VirtualTextureStreaming = Settings.bVirtualTextureStreaming;
		}

		if (UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
		{
			if (Settings.bHasAddressX)
			{
				Texture2D->AddressX = Settings.AddressX;
			}
			if (Settings.bHasAddressY)
			{
				Texture2D->AddressY = Settings.AddressY;
			}
		}

		Texture->PostEditChange();
		Texture->UpdateResource();
		Texture->MarkPackageDirty();

		if (!UEditorAssetLibrary::SaveAsset(AssetPath, false))
		{
			OutErrorMessage = FString::Printf(TEXT("Texture settings were applied, but the asset could not be saved: %s"), *AssetPath);
			return false;
		}

		return true;
	}

	static bool ExecuteQueuedTextureImport(float DeltaTime)
	{
		FString ImportId;
		FTextureImportJob Job;

		{
			FScopeLock Lock(&GTextureImportJobLock);

			if (GbTextureImportBusy)
			{
				return true;
			}

			while (GTextureImportQueue.Num() > 0)
			{
				const FString CandidateId = GTextureImportQueue[0];
				GTextureImportQueue.RemoveAt(0);

				if (FTextureImportJob* CandidateJob = GTextureImportJobs.Find(CandidateId))
				{
					if (CandidateJob->State == ETextureImportJobState::Queued)
					{
						ImportId = CandidateId;
						CandidateJob->State = ETextureImportJobState::Running;
						CandidateJob->StartedAtUtc = FDateTime::UtcNow();
						Job = *CandidateJob;
						GbTextureImportBusy = true;
						break;
					}
				}
			}

			if (ImportId.IsEmpty())
			{
				GTextureImportTickerHandle.Reset();
				return false;
			}
		}

		bool bImportSuccess = false;
		FString ImportedAssetPath = Job.DestinationPath;
		FString ErrorCode;
		FString ErrorMessage;

		if (UEditorAssetLibrary::DoesAssetExist(Job.DestinationPath))
		{
			if (!Job.bReplaceExisting)
			{
				ErrorCode = VibeUE::ErrorCodes::TEXTURE_IMPORT_FAILED;
				ErrorMessage = FString::Printf(
					TEXT("Destination asset already exists: %s. Retry with replace_existing=true or choose a different destination path."),
					*Job.DestinationPath
				);
			}
			else if (!UEditorAssetLibrary::DeleteAsset(Job.DestinationPath))
			{
				ErrorCode = VibeUE::ErrorCodes::TEXTURE_IMPORT_FAILED;
				ErrorMessage = FString::Printf(TEXT("Failed to delete existing asset before re-import: %s"), *Job.DestinationPath);
			}
		}

		if (ErrorMessage.IsEmpty())
		{
			const FString PackagePath = GetPackagePathFromAssetPath(Job.DestinationPath);
			if (!UEditorAssetLibrary::DoesDirectoryExist(PackagePath))
			{
				UEditorAssetLibrary::MakeDirectory(PackagePath);
			}

			bImportSuccess = UAssetDiscoveryService::ImportTexture(Job.SourceFilePath, Job.DestinationPath);
			if (bImportSuccess)
			{
				FAssetData ImportedAssetData;
				if (UAssetDiscoveryService::FindAssetByPath(Job.DestinationPath, ImportedAssetData))
				{
					ImportedAssetPath = ImportedAssetData.PackageName.ToString();
				}

				UObject* ImportedAsset = UEditorAssetLibrary::LoadAsset(ImportedAssetPath);
				if (!ImportedAsset)
				{
					bImportSuccess = false;
					ErrorCode = VibeUE::ErrorCodes::TEXTURE_IMPORT_FAILED;
					ErrorMessage = FString::Printf(TEXT("Texture import succeeded, but the imported asset could not be loaded: %s"), *ImportedAssetPath);
				}
				else if (!ApplyResolvedTextureImportSettings(ImportedAsset, ImportedAssetPath, Job.Settings, ErrorMessage))
				{
					bImportSuccess = false;
					ErrorCode = VibeUE::ErrorCodes::TEXTURE_IMPORT_FAILED;
				}
			}
			else
			{
				ErrorCode = VibeUE::ErrorCodes::TEXTURE_IMPORT_FAILED;
				ErrorMessage = FString::Printf(
					TEXT("ImportTexture failed for source '%s' into destination '%s'."),
					*Job.SourceFilePath,
					*Job.DestinationPath
				);
			}
		}

		{
			FScopeLock Lock(&GTextureImportJobLock);
			if (FTextureImportJob* StoredJob = GTextureImportJobs.Find(ImportId))
			{
				StoredJob->CompletedAtUtc = FDateTime::UtcNow();
				StoredJob->ImportedAssetPath = ImportedAssetPath;
				if (bImportSuccess)
				{
					StoredJob->State = ETextureImportJobState::Succeeded;
					StoredJob->ErrorCode.Reset();
					StoredJob->ErrorMessage.Reset();
				}
				else
				{
					StoredJob->State = ETextureImportJobState::Failed;
					StoredJob->ErrorCode = ErrorCode.IsEmpty() ? VibeUE::ErrorCodes::TEXTURE_IMPORT_FAILED : ErrorCode;
					StoredJob->ErrorMessage = ErrorMessage;
				}
			}

			GbTextureImportBusy = false;
			TrimTextureImportHistory_NoLock();

			for (const FString& QueuedId : GTextureImportQueue)
			{
				if (const FTextureImportJob* QueuedJob = GTextureImportJobs.Find(QueuedId))
				{
					if (QueuedJob->State == ETextureImportJobState::Queued)
					{
						return true;
					}
				}
			}
		}

		GTextureImportTickerHandle.Reset();
		return false;
	}

	static void EnsureTextureImportTickerRegistered()
	{
		if (!GTextureImportTickerHandle.IsValid())
		{
			GTextureImportTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateStatic(&ExecuteQueuedTextureImport),
				0.0f
			);
		}
	}
}

REGISTER_VIBEUE_TOOL(import_texture,
	"Import a texture into the project asynchronously. First call without import_id to enqueue an import and receive an import_id. Then poll with import_id until the status becomes succeeded or failed. Supports texture profiles and targeted overrides for compression, sampling, mip generation, texture group, sRGB, alpha, and addressing.",
	"Assets",
	TOOL_PARAMS(
		TOOL_PARAM("source_file_path", "Absolute path to the source texture file (PNG, JPG, TGA, etc.). Required when submitting a new import.", "string", false),
		TOOL_PARAM("destination_path", "Target asset path such as /Game/UI/Textures/T_Icon. Required when submitting a new import.", "string", false),
		TOOL_PARAM("import_id", "Existing import request id to poll. When provided, the tool returns the current async import status.", "string", false),
		TOOL_PARAM("replace_existing", "Whether to delete an existing destination asset before importing. Default false.", "boolean", false),
		TOOL_PARAM("profile", "Optional import profile. Supported values: default, ui, ui_pixel, ui_mask, rgba_data, normal_map.", "string", false),
		TOOL_PARAM("filter", "Optional sampling override. Supported values: default, nearest, bilinear, trilinear.", "string", false),
		TOOL_PARAM("compression_settings", "Optional compression override. Examples: default, userinterface2d, masks, normalmap, alpha, vectorrgba, bc7, hdr.", "string", false),
		TOOL_PARAM("mip_gen_settings", "Optional mip generation override. Examples: from_texture_group, no_mipmaps, simple_average, sharpen0.", "string", false),
		TOOL_PARAM("lod_group", "Optional texture group override. Examples: ui, pixels2d, world, worldnormalmap.", "string", false),
		TOOL_PARAM("srgb", "Optional sRGB override.", "boolean", false),
		TOOL_PARAM("compression_no_alpha", "Optional alpha compression override.", "boolean", false),
		TOOL_PARAM("compression_none", "Optional uncompressed override.", "boolean", false),
		TOOL_PARAM("never_stream", "Optional streaming override.", "boolean", false),
		TOOL_PARAM("virtual_texture_streaming", "Optional virtual texture streaming override.", "boolean", false),
		TOOL_PARAM("address_x", "Optional X-axis addressing override. Supported values: wrap, clamp, mirror.", "string", false),
		TOOL_PARAM("address_y", "Optional Y-axis addressing override. Supported values: wrap, clamp, mirror.", "string", false)
	),
	{
		const FString ImportId = ExtractTextureImportParam(Params, TEXT("import_id"));
		if (!ImportId.IsEmpty())
		{
			FScopeLock Lock(&GTextureImportJobLock);
			if (const FTextureImportJob* ExistingJob = GTextureImportJobs.Find(ImportId))
			{
				return SerializeTextureImportJson(BuildTextureImportJobResponse(TEXT("status"), *ExistingJob));
			}

			return SerializeTextureImportJson(BuildTextureImportError(
				VibeUE::ErrorCodes::TEXTURE_IMPORT_FAILED,
				FString::Printf(TEXT("Texture import job '%s' was not found."), *ImportId),
				ImportId
			));
		}

		const FString SourceFilePath = ExtractTextureImportParam(Params, TEXT("source_file_path"));
		const FString DestinationPath = ExtractTextureImportParam(Params, TEXT("destination_path"));
		const bool bReplaceExisting = ExtractTextureImportBool(Params, TEXT("replace_existing"), false);

		if (SourceFilePath.IsEmpty() || DestinationPath.IsEmpty())
		{
			return SerializeTextureImportJson(BuildTextureImportError(
				VibeUE::ErrorCodes::PARAM_MISSING,
				TEXT("source_file_path and destination_path are required when submitting a new texture import.")
			));
		}

		if (!FPaths::FileExists(SourceFilePath))
		{
			return SerializeTextureImportJson(BuildTextureImportError(
				VibeUE::ErrorCodes::TEXTURE_DATA_INVALID,
				FString::Printf(TEXT("Source texture file does not exist: %s"), *SourceFilePath)
			));
		}

		if (!IsSupportedTextureSource(SourceFilePath))
		{
			return SerializeTextureImportJson(BuildTextureImportError(
				VibeUE::ErrorCodes::TEXTURE_FORMAT_UNSUPPORTED,
				FString::Printf(TEXT("Unsupported source texture format for file: %s"), *SourceFilePath)
			));
		}

		if (!IsValidDestinationAssetPath(DestinationPath))
		{
			return SerializeTextureImportJson(BuildTextureImportError(
				VibeUE::ErrorCodes::PARAM_INVALID,
				FString::Printf(TEXT("Destination path must be a full /Game asset path, for example /Game/UI/Textures/T_Icon. Received: %s"), *DestinationPath)
			));
		}

		if (UEditorAssetLibrary::DoesAssetExist(DestinationPath) && !bReplaceExisting)
		{
			return SerializeTextureImportJson(BuildTextureImportError(
				VibeUE::ErrorCodes::TEXTURE_IMPORT_FAILED,
				FString::Printf(TEXT("Destination asset already exists: %s. Set replace_existing=true or choose a different destination path."), *DestinationPath)
			));
		}

		FTextureImportSettings Settings;
		FString SettingsErrorCode;
		FString SettingsErrorMessage;
		if (!ResolveTextureImportSettings(Params, Settings, SettingsErrorCode, SettingsErrorMessage))
		{
			return SerializeTextureImportJson(BuildTextureImportError(SettingsErrorCode, SettingsErrorMessage));
		}

		const FString NewImportId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		FTextureImportJob NewJob;
		NewJob.ImportId = NewImportId;
		NewJob.SourceFilePath = SourceFilePath;
		NewJob.DestinationPath = DestinationPath;
		NewJob.bReplaceExisting = bReplaceExisting;
		NewJob.Settings = Settings;
		NewJob.State = ETextureImportJobState::Queued;
		NewJob.SubmittedAtUtc = FDateTime::UtcNow();

		{
			FScopeLock Lock(&GTextureImportJobLock);
			GTextureImportJobs.Add(NewImportId, NewJob);
			GTextureImportQueue.Add(NewImportId);
			TrimTextureImportHistory_NoLock();
		}

		EnsureTextureImportTickerRegistered();
		return SerializeTextureImportJson(BuildTextureImportJobResponse(TEXT("submit"), NewJob));
	}
);
