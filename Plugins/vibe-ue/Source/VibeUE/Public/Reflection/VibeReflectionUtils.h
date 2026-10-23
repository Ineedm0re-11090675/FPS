// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class FProperty;
class UStruct;
class UObject;

struct VIBEUE_API FResolvedObjectTarget
{
	UObject* TargetObject = nullptr;
	UObject* SaveObject = nullptr;
	FString RequestedTarget;
	FString ResolvedKind;
	FString ResolvedObjectPath;
	bool bIsBlueprintCDO = false;
};

struct VIBEUE_API FResolvedPropertyPath
{
	FProperty* Property = nullptr;
	void* ValuePtr = nullptr;
	FString CanonicalPath;
};

class VIBEUE_API FVibeReflectionUtils
{
public:
	static bool ResolveTarget(const FString& Target, FResolvedObjectTarget& OutTarget, FString& OutError);
	static bool ResolvePropertyPath(UObject* RootObject, const FString& Path, FResolvedPropertyPath& OutResolved, FString& OutError);
	static FString GetPropertyTypeString(FProperty* Property);

	static TSharedPtr<FJsonObject> BuildObjectSchemaJson(
		UObject* TargetObject,
		bool bIncludeInherited,
		int32 MaxDepth);

	static TSharedPtr<FJsonObject> BuildTypedValueJson(
		FProperty* Property,
		const void* ValuePtr,
		int32 MaxDepth = 4);

	static TSharedPtr<FJsonObject> BuildStructValueJson(
		const UScriptStruct* StructType,
		const void* StructData,
		int32 MaxDepth = 4);

	static bool SetPropertyValueFromJson(
		FProperty* Property,
		void* ValuePtr,
		const TSharedPtr<FJsonValue>& JsonValue,
		FString& OutError,
		int32 MaxDepth = 8);

	static bool SetStructValueFromJson(
		const UScriptStruct* StructType,
		void* StructData,
		const TSharedPtr<FJsonValue>& JsonValue,
		FString& OutError,
		int32 MaxDepth = 8);

	static bool SaveResolvedTarget(const FResolvedObjectTarget& Target, bool bCompileBlueprint, FString& OutError);
};
