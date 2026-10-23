// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Reflection/VibeReflectionUtils.h"

#include "Core/ErrorCodes.h"
#include "Core/JsonValueHelper.h"

#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Json.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace
{
	struct FPropertyPathToken
	{
		FString Name;
		TOptional<int32> ArrayIndex;
	};

	static UClass* TryLoadClassByPath(const FString& ClassPath)
	{
		if (ClassPath.IsEmpty())
		{
			return nullptr;
		}

		if (UClass* FoundClass = FindObject<UClass>(nullptr, *ClassPath))
		{
			return FoundClass;
		}

		if (UClass* LoadedClass = LoadObject<UClass>(nullptr, *ClassPath))
		{
			return LoadedClass;
		}

		return LoadClass<UObject>(nullptr, *ClassPath);
	}

	static UObject* TryLoadObjectByPath(const FString& ObjectPath)
	{
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}

		if (UObject* FoundObject = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return FoundObject;
		}

		if (UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(ObjectPath))
		{
			return LoadedAsset;
		}

		return LoadObject<UObject>(nullptr, *ObjectPath);
	}

	static UObject* GetSaveObjectForTarget(UObject* TargetObject, bool& bOutIsBlueprintCDO)
	{
		bOutIsBlueprintCDO = false;

		if (!TargetObject)
		{
			return nullptr;
		}

		if (UBlueprint* Blueprint = Cast<UBlueprint>(TargetObject))
		{
			return Blueprint;
		}

		if (UClass* ClassObject = Cast<UClass>(TargetObject))
		{
			if (UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(ClassObject))
			{
				bOutIsBlueprintCDO = true;
				return Cast<UBlueprint>(GeneratedClass->ClassGeneratedBy);
			}
			return ClassObject;
		}

		if (TargetObject->HasAnyFlags(RF_ClassDefaultObject))
		{
			if (UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(TargetObject->GetClass()))
			{
				bOutIsBlueprintCDO = true;
				return Cast<UBlueprint>(GeneratedClass->ClassGeneratedBy);
			}
		}

		return TargetObject;
	}

	static bool ShouldExposeProperty(FProperty* Property)
	{
		if (!Property)
		{
			return false;
		}

		if (Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient))
		{
			return false;
		}

		return true;
	}

	static bool IsWritableProperty(FProperty* Property)
	{
		if (!Property)
		{
			return false;
		}

		if (Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient | CPF_EditConst))
		{
			return false;
		}

		return true;
	}

	static FProperty* FindPropertyByName(UStruct* StructType, const FString& Name)
	{
		if (!StructType || Name.IsEmpty())
		{
			return nullptr;
		}

		if (FProperty* Exact = FindFProperty<FProperty>(StructType, *Name))
		{
			return Exact;
		}

		for (TFieldIterator<FProperty> It(StructType, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (Property && Property->GetName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Property;
			}
		}

		return nullptr;
	}

	static TArray<FPropertyPathToken> ParsePropertyPath(const FString& Path)
	{
		TArray<FPropertyPathToken> Tokens;
		FString Working = Path.TrimStartAndEnd();
		if (Working.IsEmpty())
		{
			return Tokens;
		}

		TArray<FString> Segments;
		Working.ParseIntoArray(Segments, TEXT("."));
		for (const FString& Segment : Segments)
		{
			FPropertyPathToken Token;
			int32 BracketIndex = INDEX_NONE;
			if (Segment.FindChar(TEXT('['), BracketIndex))
			{
				Token.Name = Segment.Left(BracketIndex);
				const int32 CloseBracket = Segment.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, BracketIndex + 1);
				if (CloseBracket > BracketIndex + 1)
				{
					const FString IndexText = Segment.Mid(BracketIndex + 1, CloseBracket - BracketIndex - 1);
					Token.ArrayIndex = FCString::Atoi(*IndexText);
				}
			}
			else
			{
				Token.Name = Segment;
			}

			Token.Name = Token.Name.TrimStartAndEnd();
			if (!Token.Name.IsEmpty())
			{
				Tokens.Add(Token);
			}
		}

		return Tokens;
	}

	static FString JsonValueToCompactString(const TSharedPtr<FJsonValue>& JsonValue)
	{
		if (!JsonValue.IsValid())
		{
			return TEXT("null");
		}

		FString OutString;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutString);
		FJsonSerializer::Serialize(JsonValue.ToSharedRef(), TEXT(""), Writer);
		return OutString;
	}

	static TSharedPtr<FJsonValue> GetEnvelopeFieldOrSelf(const TSharedPtr<FJsonValue>& JsonValue, const FString& FieldName)
	{
		TSharedPtr<FJsonObject> ObjectValue;
		if (FJsonValueHelper::TryGetObject(JsonValue, ObjectValue) && ObjectValue.IsValid())
		{
			if (ObjectValue->HasField(TEXT("kind")) && ObjectValue->HasField(FieldName))
			{
				return ObjectValue->TryGetField(FieldName);
			}
		}

		return JsonValue;
	}

	static bool GetStringLikeValue(const TSharedPtr<FJsonValue>& JsonValue, FString& OutValue)
	{
		const TSharedPtr<FJsonValue> Candidate = GetEnvelopeFieldOrSelf(JsonValue, TEXT("value"));
		if (!Candidate.IsValid())
		{
			return false;
		}

		if (FJsonValueHelper::TryGetString(Candidate, OutValue))
		{
			return true;
		}

		if (Candidate->Type == EJson::Null)
		{
			OutValue.Reset();
			return true;
		}

		return false;
	}

	static bool GetNumberLikeValue(const TSharedPtr<FJsonValue>& JsonValue, double& OutValue)
	{
		const TSharedPtr<FJsonValue> Candidate = GetEnvelopeFieldOrSelf(JsonValue, TEXT("value"));
		if (!Candidate.IsValid())
		{
			return false;
		}

		if (Candidate->TryGetNumber(OutValue))
		{
			return true;
		}

		FString StringValue;
		if (FJsonValueHelper::TryGetString(Candidate, StringValue))
		{
			return LexTryParseString(OutValue, *StringValue);
		}

		return false;
	}

	static bool GetBoolLikeValue(const TSharedPtr<FJsonValue>& JsonValue, bool& OutValue)
	{
		const TSharedPtr<FJsonValue> Candidate = GetEnvelopeFieldOrSelf(JsonValue, TEXT("value"));
		return Candidate.IsValid() && FJsonValueHelper::TryGetBool(Candidate, OutValue);
	}

	static UClass* ResolveClassReferenceFromJson(const TSharedPtr<FJsonValue>& JsonValue, FString& OutError)
	{
		if (!JsonValue.IsValid())
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> ObjectValue;
		FString ClassPath;
		if (FJsonValueHelper::TryGetObject(JsonValue, ObjectValue) && ObjectValue.IsValid())
		{
			ObjectValue->TryGetStringField(TEXT("class_path"), ClassPath);
			if (ClassPath.IsEmpty())
			{
				ObjectValue->TryGetStringField(TEXT("value"), ClassPath);
			}
		}
		else
		{
			FJsonValueHelper::TryGetString(JsonValue, ClassPath);
		}

		if (ClassPath.IsEmpty())
		{
			OutError = TEXT("Expected a class reference with class_path or string path.");
			return nullptr;
		}

		if (UClass* LoadedClass = TryLoadClassByPath(ClassPath))
		{
			return LoadedClass;
		}

		OutError = FString::Printf(TEXT("Failed to load class '%s'."), *ClassPath);
		return nullptr;
	}

	static UObject* ResolveObjectReferenceFromJson(const TSharedPtr<FJsonValue>& JsonValue, FString& OutError)
	{
		if (!JsonValue.IsValid())
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> ObjectValue;
		FString ObjectPath;
		if (FJsonValueHelper::TryGetObject(JsonValue, ObjectValue) && ObjectValue.IsValid())
		{
			ObjectValue->TryGetStringField(TEXT("object_path"), ObjectPath);
			if (ObjectPath.IsEmpty())
			{
				ObjectValue->TryGetStringField(TEXT("value"), ObjectPath);
			}
		}
		else
		{
			FJsonValueHelper::TryGetString(JsonValue, ObjectPath);
		}

		if (ObjectPath.IsEmpty())
		{
			OutError = TEXT("Expected an object reference with object_path or string path.");
			return nullptr;
		}

		if (UObject* LoadedObject = TryLoadObjectByPath(ObjectPath))
		{
			return LoadedObject;
		}

		OutError = FString::Printf(TEXT("Failed to load object '%s'."), *ObjectPath);
		return nullptr;
	}

	static bool SetNumericProperty(FProperty* Property, void* ValuePtr, double NumericValue, FString& OutError)
	{
		if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			if (NumericProperty->IsInteger())
			{
				NumericProperty->SetIntPropertyValue(ValuePtr, static_cast<int64>(NumericValue));
				return true;
			}

			if (NumericProperty->IsFloatingPoint())
			{
				NumericProperty->SetFloatingPointPropertyValue(ValuePtr, NumericValue);
				return true;
			}
		}

		OutError = FString::Printf(TEXT("Property '%s' is not numeric."), *Property->GetName());
		return false;
	}

	static void AppendSchemaEntries(
		TArray<TSharedPtr<FJsonValue>>& OutArray,
		UStruct* StructType,
		const FString& Prefix,
		bool bIncludeInherited,
		int32 RemainingDepth)
	{
		if (!StructType || RemainingDepth <= 0)
		{
			return;
		}

		const EFieldIterationFlags IterationFlags = bIncludeInherited ? EFieldIterationFlags::IncludeSuper : EFieldIterationFlags::None;
		for (TFieldIterator<FProperty> It(StructType, IterationFlags); It; ++It)
		{
			FProperty* Property = *It;
			if (!ShouldExposeProperty(Property))
			{
				continue;
			}

			const FString PropertyPath = Prefix.IsEmpty() ? Property->GetName() : Prefix + TEXT(".") + Property->GetName();

			const TSharedPtr<FJsonObject> PropertyObj = MakeShared<FJsonObject>();
			PropertyObj->SetStringField(TEXT("name"), Property->GetName());
			PropertyObj->SetStringField(TEXT("path"), PropertyPath);
			PropertyObj->SetStringField(TEXT("type"), FVibeReflectionUtils::GetPropertyTypeString(Property));
			PropertyObj->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
			PropertyObj->SetBoolField(TEXT("writable"), IsWritableProperty(Property));

			if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				PropertyObj->SetStringField(TEXT("struct_path"), StructProperty->Struct ? StructProperty->Struct->GetPathName() : FString());
				if (RemainingDepth > 1)
				{
					TArray<TSharedPtr<FJsonValue>> ChildProps;
					AppendSchemaEntries(ChildProps, StructProperty->Struct, PropertyPath, bIncludeInherited, RemainingDepth - 1);
					PropertyObj->SetArrayField(TEXT("children"), ChildProps);
				}
			}
			else if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
			{
				PropertyObj->SetStringField(TEXT("element_type"), FVibeReflectionUtils::GetPropertyTypeString(ArrayProperty->Inner));
				if (const FStructProperty* InnerStruct = CastField<FStructProperty>(ArrayProperty->Inner))
				{
					PropertyObj->SetStringField(TEXT("element_struct_path"), InnerStruct->Struct ? InnerStruct->Struct->GetPathName() : FString());
					if (RemainingDepth > 1)
					{
						TArray<TSharedPtr<FJsonValue>> ChildProps;
						AppendSchemaEntries(ChildProps, InnerStruct->Struct, PropertyPath + TEXT("[0]"), bIncludeInherited, RemainingDepth - 1);
						PropertyObj->SetArrayField(TEXT("children"), ChildProps);
					}
				}
			}

			OutArray.Add(MakeShared<FJsonValueObject>(PropertyObj));
		}
	}
}

bool FVibeReflectionUtils::ResolveTarget(const FString& Target, FResolvedObjectTarget& OutTarget, FString& OutError)
{
	OutTarget = FResolvedObjectTarget();
	OutTarget.RequestedTarget = Target;

	const FString TrimmedTarget = Target.TrimStartAndEnd();
	if (TrimmedTarget.IsEmpty())
	{
		OutError = TEXT("Target cannot be empty.");
		return false;
	}

	auto AssignResolved = [&](UObject* ResolvedObject, const FString& ResolvedKind) -> bool
	{
		if (!ResolvedObject)
		{
			return false;
		}

		OutTarget.TargetObject = ResolvedObject;
		OutTarget.ResolvedKind = ResolvedKind;
		OutTarget.ResolvedObjectPath = ResolvedObject->GetPathName();
		OutTarget.SaveObject = GetSaveObjectForTarget(ResolvedObject, OutTarget.bIsBlueprintCDO);
		return true;
	};

	if (TrimmedTarget.StartsWith(TEXT("cdo:"), ESearchCase::IgnoreCase))
	{
		const FString ClassPath = TrimmedTarget.RightChop(4);
		if (UClass* TargetClass = TryLoadClassByPath(ClassPath))
		{
			if (AssignResolved(TargetClass->GetDefaultObject(), TEXT("cdo")))
			{
				return true;
			}
		}

		OutError = FString::Printf(TEXT("Failed to resolve CDO target '%s'."), *ClassPath);
		return false;
	}

	if (TrimmedTarget.StartsWith(TEXT("class:"), ESearchCase::IgnoreCase))
	{
		const FString ClassPath = TrimmedTarget.RightChop(6);
		if (UClass* TargetClass = TryLoadClassByPath(ClassPath))
		{
			if (AssignResolved(TargetClass, TEXT("class")))
			{
				return true;
			}
		}

		OutError = FString::Printf(TEXT("Failed to resolve class target '%s'."), *ClassPath);
		return false;
	}

	if (TrimmedTarget.StartsWith(TEXT("object:"), ESearchCase::IgnoreCase) ||
		TrimmedTarget.StartsWith(TEXT("asset:"), ESearchCase::IgnoreCase))
	{
		const FString ObjectPath = TrimmedTarget.RightChop(TrimmedTarget.Find(TEXT(":")) + 1);
		if (AssignResolved(TryLoadObjectByPath(ObjectPath), TEXT("object")))
		{
			return true;
		}

		OutError = FString::Printf(TEXT("Failed to resolve object target '%s'."), *ObjectPath);
		return false;
	}

	if (AssignResolved(TryLoadObjectByPath(TrimmedTarget), TEXT("object")))
	{
		return true;
	}

	if (UClass* TargetClass = TryLoadClassByPath(TrimmedTarget))
	{
		if (AssignResolved(TargetClass->GetDefaultObject(), TEXT("cdo")))
		{
			return true;
		}
	}

	OutError = FString::Printf(TEXT("Failed to resolve target '%s'. Use object:/ asset:/ class:/ or cdo: prefixes for ambiguous cases."), *TrimmedTarget);
	return false;
}

bool FVibeReflectionUtils::ResolvePropertyPath(UObject* RootObject, const FString& Path, FResolvedPropertyPath& OutResolved, FString& OutError)
{
	OutResolved = FResolvedPropertyPath();

	if (!RootObject)
	{
		OutError = TEXT("Root object is null.");
		return false;
	}

	const TArray<FPropertyPathToken> Tokens = ParsePropertyPath(Path);
	if (Tokens.Num() == 0)
	{
		OutError = TEXT("Property path is empty or invalid.");
		return false;
	}

	UObject* CurrentObject = RootObject;
	UStruct* CurrentStruct = RootObject->GetClass();
	void* CurrentContainer = RootObject;
	FString CanonicalPath;

	for (int32 TokenIndex = 0; TokenIndex < Tokens.Num(); ++TokenIndex)
	{
		const FPropertyPathToken& Token = Tokens[TokenIndex];
		FProperty* Property = FindPropertyByName(CurrentStruct, Token.Name);
		if (!Property)
		{
			OutError = FString::Printf(TEXT("Property '%s' was not found on '%s'."), *Token.Name, *CurrentStruct->GetName());
			return false;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
		FProperty* EffectiveProperty = Property;
		void* EffectiveValuePtr = ValuePtr;

		if (Token.ArrayIndex.IsSet())
		{
			FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);
			if (!ArrayProperty)
			{
				OutError = FString::Printf(TEXT("Property '%s' is not an array."), *Property->GetName());
				return false;
			}

			FScriptArrayHelper ArrayHelper(ArrayProperty, ValuePtr);
			const int32 ArrayIndex = Token.ArrayIndex.GetValue();
			if (!ArrayHelper.IsValidIndex(ArrayIndex))
			{
				OutError = FString::Printf(TEXT("Array index %d is out of bounds for '%s'."), ArrayIndex, *Property->GetName());
				return false;
			}

			EffectiveProperty = ArrayProperty->Inner;
			EffectiveValuePtr = ArrayHelper.GetRawPtr(ArrayIndex);
			CanonicalPath = CanonicalPath.IsEmpty()
				? FString::Printf(TEXT("%s[%d]"), *Property->GetName(), ArrayIndex)
				: CanonicalPath + TEXT(".") + FString::Printf(TEXT("%s[%d]"), *Property->GetName(), ArrayIndex);
		}
		else
		{
			CanonicalPath = CanonicalPath.IsEmpty() ? Property->GetName() : CanonicalPath + TEXT(".") + Property->GetName();
		}

		const bool bIsLastToken = TokenIndex == Tokens.Num() - 1;
		if (bIsLastToken)
		{
			OutResolved.Property = EffectiveProperty;
			OutResolved.ValuePtr = EffectiveValuePtr;
			OutResolved.CanonicalPath = CanonicalPath;
			return true;
		}

		if (FStructProperty* StructProperty = CastField<FStructProperty>(EffectiveProperty))
		{
			CurrentStruct = StructProperty->Struct;
			CurrentContainer = EffectiveValuePtr;
			continue;
		}

		if (FObjectProperty* ObjectProperty = CastField<FObjectProperty>(EffectiveProperty))
		{
			CurrentObject = ObjectProperty->GetObjectPropertyValue(EffectiveValuePtr);
			if (!CurrentObject)
			{
				OutError = FString::Printf(TEXT("Object property '%s' is null."), *EffectiveProperty->GetName());
				return false;
			}

			CurrentStruct = CurrentObject->GetClass();
			CurrentContainer = CurrentObject;
			continue;
		}

		OutError = FString::Printf(TEXT("Cannot descend through property '%s' of type '%s'."), *EffectiveProperty->GetName(), *GetPropertyTypeString(EffectiveProperty));
		return false;
	}

	OutError = FString::Printf(TEXT("Failed to resolve property path '%s'."), *Path);
	return false;
}

FString FVibeReflectionUtils::GetPropertyTypeString(FProperty* Property)
{
	if (!Property)
	{
		return TEXT("unknown");
	}

	if (CastField<FBoolProperty>(Property)) return TEXT("bool");
	if (CastField<FNameProperty>(Property)) return TEXT("name");
	if (CastField<FTextProperty>(Property)) return TEXT("text");
	if (CastField<FStrProperty>(Property)) return TEXT("string");

	if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
	{
		return NumericProperty->IsInteger() ? TEXT("int") : TEXT("float");
	}

	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		return FString::Printf(TEXT("enum:%s"), EnumProperty->GetEnum() ? *EnumProperty->GetEnum()->GetPathName() : TEXT("unknown"));
	}

	if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
	{
		if (ByteProperty->Enum)
		{
			return FString::Printf(TEXT("enum:%s"), *ByteProperty->Enum->GetPathName());
		}
		return TEXT("byte");
	}

	if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
	{
		return FString::Printf(TEXT("class:%s"), *ClassProperty->MetaClass->GetPathName());
	}

	if (const FSoftClassProperty* SoftClassProperty = CastField<FSoftClassProperty>(Property))
	{
		return FString::Printf(TEXT("soft_class:%s"), *SoftClassProperty->MetaClass->GetPathName());
	}

	if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
	{
		return FString::Printf(TEXT("object:%s"), *ObjectProperty->PropertyClass->GetPathName());
	}

	if (const FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
	{
		return FString::Printf(TEXT("soft_object:%s"), *SoftObjectProperty->PropertyClass->GetPathName());
	}

	if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		return FString::Printf(TEXT("struct:%s"), StructProperty->Struct ? *StructProperty->Struct->GetPathName() : TEXT("unknown"));
	}

	if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		return FString::Printf(TEXT("array<%s>"), *GetPropertyTypeString(ArrayProperty->Inner));
	}

	if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
	{
		return FString::Printf(TEXT("map<%s,%s>"), *GetPropertyTypeString(MapProperty->KeyProp), *GetPropertyTypeString(MapProperty->ValueProp));
	}

	if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
	{
		return FString::Printf(TEXT("set<%s>"), *GetPropertyTypeString(SetProperty->ElementProp));
	}

	return Property->GetCPPType();
}

TSharedPtr<FJsonObject> FVibeReflectionUtils::BuildObjectSchemaJson(UObject* TargetObject, bool bIncludeInherited, int32 MaxDepth)
{
	const TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
	if (!TargetObject)
	{
		return RootObj;
	}

	RootObj->SetStringField(TEXT("target_name"), TargetObject->GetName());
	RootObj->SetStringField(TEXT("target_path"), TargetObject->GetPathName());
	RootObj->SetStringField(TEXT("target_class"), TargetObject->GetClass()->GetPathName());

	TArray<TSharedPtr<FJsonValue>> PropertiesArray;
	AppendSchemaEntries(PropertiesArray, TargetObject->GetClass(), FString(), bIncludeInherited, FMath::Max(1, MaxDepth));
	RootObj->SetArrayField(TEXT("properties"), PropertiesArray);
	return RootObj;
}

TSharedPtr<FJsonObject> FVibeReflectionUtils::BuildTypedValueJson(FProperty* Property, const void* ValuePtr, int32 MaxDepth)
{
	const TSharedPtr<FJsonObject> ValueObj = MakeShared<FJsonObject>();
	if (!Property || !ValuePtr)
	{
		ValueObj->SetStringField(TEXT("kind"), TEXT("null"));
		return ValueObj;
	}

		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
	{
		ValueObj->SetStringField(TEXT("kind"), TEXT("bool"));
		ValueObj->SetBoolField(TEXT("value"), BoolProperty->GetPropertyValue(ValuePtr));
		return ValueObj;
	}

	if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
	{
		if (NumericProperty->IsInteger())
		{
			ValueObj->SetStringField(TEXT("kind"), TEXT("int"));
			ValueObj->SetStringField(TEXT("value"), FString::Printf(TEXT("%lld"), NumericProperty->GetSignedIntPropertyValue(ValuePtr)));
		}
		else
		{
			ValueObj->SetStringField(TEXT("kind"), TEXT("float"));
			ValueObj->SetNumberField(TEXT("value"), NumericProperty->GetFloatingPointPropertyValue(ValuePtr));
		}
		return ValueObj;
	}

	if (const FStrProperty* StringProperty = CastField<FStrProperty>(Property))
	{
		ValueObj->SetStringField(TEXT("kind"), TEXT("string"));
		ValueObj->SetStringField(TEXT("value"), StringProperty->GetPropertyValue(ValuePtr));
		return ValueObj;
	}

	if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
	{
		ValueObj->SetStringField(TEXT("kind"), TEXT("name"));
		ValueObj->SetStringField(TEXT("value"), NameProperty->GetPropertyValue(ValuePtr).ToString());
		return ValueObj;
	}

	if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
	{
		ValueObj->SetStringField(TEXT("kind"), TEXT("text"));
		ValueObj->SetStringField(TEXT("value"), TextProperty->GetPropertyValue(ValuePtr).ToString());
		return ValueObj;
	}

	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		ValueObj->SetStringField(TEXT("kind"), TEXT("enum"));
		if (UEnum* Enum = EnumProperty->GetEnum())
		{
			const int64 RawValue = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
			ValueObj->SetStringField(TEXT("enum_path"), Enum->GetPathName());
			ValueObj->SetStringField(TEXT("value"), Enum->GetNameStringByValue(RawValue));
		}
		return ValueObj;
	}

	if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
	{
		if (UEnum* Enum = ByteProperty->Enum)
		{
			ValueObj->SetStringField(TEXT("kind"), TEXT("enum"));
			ValueObj->SetStringField(TEXT("enum_path"), Enum->GetPathName());
			ValueObj->SetStringField(TEXT("value"), Enum->GetNameStringByValue(ByteProperty->GetPropertyValue(ValuePtr)));
		}
		else
		{
			ValueObj->SetStringField(TEXT("kind"), TEXT("byte"));
			ValueObj->SetNumberField(TEXT("value"), ByteProperty->GetPropertyValue(ValuePtr));
		}
		return ValueObj;
	}

	if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
	{
		ValueObj->SetStringField(TEXT("kind"), TEXT("class"));
		if (UClass* ClassValue = Cast<UClass>(ClassProperty->GetObjectPropertyValue(ValuePtr)))
		{
			ValueObj->SetStringField(TEXT("class_path"), ClassValue->GetPathName());
		}
		return ValueObj;
	}

	if (const FSoftClassProperty* SoftClassProperty = CastField<FSoftClassProperty>(Property))
	{
		ValueObj->SetStringField(TEXT("kind"), TEXT("soft_class"));
		FString ExportedValue;
		SoftClassProperty->ExportTextItem_Direct(ExportedValue, ValuePtr, nullptr, nullptr, PPF_None);
		ValueObj->SetStringField(TEXT("class_path"), ExportedValue);
		return ValueObj;
	}

	if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
	{
		ValueObj->SetStringField(TEXT("kind"), TEXT("object"));
		if (UObject* ObjectValue = ObjectProperty->GetObjectPropertyValue(ValuePtr))
		{
			ValueObj->SetStringField(TEXT("object_path"), ObjectValue->GetPathName());
			ValueObj->SetStringField(TEXT("object_class"), ObjectValue->GetClass()->GetPathName());
		}
		return ValueObj;
	}

	if (const FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
	{
		ValueObj->SetStringField(TEXT("kind"), TEXT("soft_object"));
		FString ExportedValue;
		SoftObjectProperty->ExportTextItem_Direct(ExportedValue, ValuePtr, nullptr, nullptr, PPF_None);
		ValueObj->SetStringField(TEXT("object_path"), ExportedValue);
		return ValueObj;
	}

	if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		return BuildStructValueJson(StructProperty->Struct, ValuePtr, MaxDepth);
	}

	if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		ValueObj->SetStringField(TEXT("kind"), TEXT("array"));
		ValueObj->SetStringField(TEXT("element_type"), GetPropertyTypeString(ArrayProperty->Inner));

		TArray<TSharedPtr<FJsonValue>> Items;
		FScriptArrayHelper ArrayHelper(ArrayProperty, ValuePtr);
		for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
		{
			Items.Add(MakeShared<FJsonValueObject>(BuildTypedValueJson(ArrayProperty->Inner, ArrayHelper.GetRawPtr(Index), MaxDepth - 1)));
		}
		ValueObj->SetArrayField(TEXT("items"), Items);
		return ValueObj;
	}

	if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
	{
		ValueObj->SetStringField(TEXT("kind"), TEXT("set"));
		TArray<TSharedPtr<FJsonValue>> Items;
		FScriptSetHelper SetHelper(SetProperty, ValuePtr);
		for (int32 Index = 0; Index < SetHelper.GetMaxIndex(); ++Index)
		{
			if (SetHelper.IsValidIndex(Index))
			{
				Items.Add(MakeShared<FJsonValueObject>(BuildTypedValueJson(SetProperty->ElementProp, SetHelper.GetElementPtr(Index), MaxDepth - 1)));
			}
		}
		ValueObj->SetArrayField(TEXT("items"), Items);
		return ValueObj;
	}

	if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
	{
		ValueObj->SetStringField(TEXT("kind"), TEXT("map"));
		TArray<TSharedPtr<FJsonValue>> Entries;
		FScriptMapHelper MapHelper(MapProperty, ValuePtr);
		for (int32 Index = 0; Index < MapHelper.GetMaxIndex(); ++Index)
		{
			if (MapHelper.IsValidIndex(Index))
			{
				const TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
				EntryObj->SetObjectField(TEXT("key"), BuildTypedValueJson(MapProperty->KeyProp, MapHelper.GetKeyPtr(Index), MaxDepth - 1));
				EntryObj->SetObjectField(TEXT("value"), BuildTypedValueJson(MapProperty->ValueProp, MapHelper.GetValuePtr(Index), MaxDepth - 1));
				Entries.Add(MakeShared<FJsonValueObject>(EntryObj));
			}
		}
		ValueObj->SetArrayField(TEXT("entries"), Entries);
		return ValueObj;
	}

	ValueObj->SetStringField(TEXT("kind"), TEXT("fallback"));
	FString ExportedValue;
	Property->ExportTextItem_Direct(ExportedValue, ValuePtr, nullptr, nullptr, PPF_None);
	ValueObj->SetStringField(TEXT("value"), ExportedValue);
	return ValueObj;
}

TSharedPtr<FJsonObject> FVibeReflectionUtils::BuildStructValueJson(const UScriptStruct* StructType, const void* StructData, int32 MaxDepth)
{
	const TSharedPtr<FJsonObject> ValueObj = MakeShared<FJsonObject>();
	ValueObj->SetStringField(TEXT("kind"), TEXT("struct"));
	ValueObj->SetStringField(TEXT("struct_path"), StructType ? StructType->GetPathName() : FString());

	if (!StructType || !StructData || MaxDepth <= 0)
	{
		return ValueObj;
	}

	const TSharedPtr<FJsonObject> FieldsObj = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(StructType, EFieldIterationFlags::None); It; ++It)
	{
		FProperty* FieldProperty = *It;
		if (!ShouldExposeProperty(FieldProperty))
		{
			continue;
		}
		const void* FieldValuePtr = FieldProperty->ContainerPtrToValuePtr<void>(StructData);
		FieldsObj->SetObjectField(FieldProperty->GetName(), BuildTypedValueJson(FieldProperty, FieldValuePtr, MaxDepth - 1));
	}
	ValueObj->SetObjectField(TEXT("fields"), FieldsObj);
	return ValueObj;
}

bool FVibeReflectionUtils::SetPropertyValueFromJson(
	FProperty* Property,
	void* ValuePtr,
	const TSharedPtr<FJsonValue>& JsonValue,
	FString& OutError,
	int32 MaxDepth)
{
	if (!Property || !ValuePtr)
	{
		OutError = TEXT("Invalid property target.");
		return false;
	}

	if (MaxDepth <= 0)
	{
		OutError = TEXT("Maximum recursion depth reached while setting property.");
		return false;
	}

	if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
	{
		bool bValue = false;
		if (!GetBoolLikeValue(JsonValue, bValue))
		{
			OutError = FString::Printf(TEXT("Failed to parse bool for property '%s'."), *Property->GetName());
			return false;
		}
		const_cast<FBoolProperty*>(BoolProperty)->SetPropertyValue(ValuePtr, bValue);
		return true;
	}

	if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
	{
		double NumberValue = 0.0;
		if (!GetNumberLikeValue(JsonValue, NumberValue))
		{
			OutError = FString::Printf(TEXT("Failed to parse numeric value for property '%s'."), *Property->GetName());
			return false;
		}
		return SetNumericProperty(NumericProperty, ValuePtr, NumberValue, OutError);
	}

	if (FStrProperty* StringProperty = CastField<FStrProperty>(Property))
	{
		FString ParsedValue;
		if (!GetStringLikeValue(JsonValue, ParsedValue))
		{
			OutError = FString::Printf(TEXT("Failed to parse string value for property '%s'."), *Property->GetName());
			return false;
		}
		StringProperty->SetPropertyValue(ValuePtr, ParsedValue);
		return true;
	}

	if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
	{
		FString ParsedValue;
		if (!GetStringLikeValue(JsonValue, ParsedValue))
		{
			OutError = FString::Printf(TEXT("Failed to parse name value for property '%s'."), *Property->GetName());
			return false;
		}
		NameProperty->SetPropertyValue(ValuePtr, FName(*ParsedValue));
		return true;
	}

	if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
	{
		FString ParsedValue;
		if (!GetStringLikeValue(JsonValue, ParsedValue))
		{
			OutError = FString::Printf(TEXT("Failed to parse text value for property '%s'."), *Property->GetName());
			return false;
		}
		TextProperty->SetPropertyValue(ValuePtr, FText::FromString(ParsedValue));
		return true;
	}

	if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		FString ParsedValue;
		if (!GetStringLikeValue(JsonValue, ParsedValue))
		{
			OutError = FString::Printf(TEXT("Failed to parse enum value for property '%s'."), *Property->GetName());
			return false;
		}

		if (UEnum* Enum = EnumProperty->GetEnum())
		{
			const int64 EnumValue = Enum->GetValueByNameString(ParsedValue);
			if (EnumValue == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("Enum '%s' does not contain value '%s'."), *Enum->GetName(), *ParsedValue);
				return false;
			}
			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, EnumValue);
			return true;
		}
	}

	if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
	{
		if (UEnum* Enum = ByteProperty->Enum)
		{
			FString ParsedValue;
			if (!GetStringLikeValue(JsonValue, ParsedValue))
			{
				OutError = FString::Printf(TEXT("Failed to parse enum byte value for property '%s'."), *Property->GetName());
				return false;
			}

			const int64 EnumValue = Enum->GetValueByNameString(ParsedValue);
			if (EnumValue == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("Enum '%s' does not contain value '%s'."), *Enum->GetName(), *ParsedValue);
				return false;
			}
			ByteProperty->SetPropertyValue(ValuePtr, static_cast<uint8>(EnumValue));
			return true;
		}

		double NumberValue = 0.0;
		if (!GetNumberLikeValue(JsonValue, NumberValue))
		{
			OutError = FString::Printf(TEXT("Failed to parse byte value for property '%s'."), *Property->GetName());
			return false;
		}
		ByteProperty->SetPropertyValue(ValuePtr, static_cast<uint8>(NumberValue));
		return true;
	}

	if (FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
	{
		if (!JsonValue.IsValid() || JsonValue->Type == EJson::Null)
		{
			ClassProperty->SetPropertyValue(ValuePtr, nullptr);
			return true;
		}

		FString ClassError;
		UClass* ClassValue = ResolveClassReferenceFromJson(JsonValue, ClassError);
		if (!ClassValue)
		{
			OutError = ClassError;
			return false;
		}

		if (ClassProperty->MetaClass && !ClassValue->IsChildOf(ClassProperty->MetaClass))
		{
			OutError = FString::Printf(TEXT("Class '%s' is not a child of '%s'."), *ClassValue->GetName(), *ClassProperty->MetaClass->GetName());
			return false;
		}

		ClassProperty->SetPropertyValue(ValuePtr, ClassValue);
		return true;
	}

	if (FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
	{
		if (!JsonValue.IsValid() || JsonValue->Type == EJson::Null)
		{
			ObjectProperty->SetObjectPropertyValue(ValuePtr, nullptr);
			return true;
		}

		FString ObjectError;
		UObject* ObjectValue = ResolveObjectReferenceFromJson(JsonValue, ObjectError);
		if (!ObjectValue)
		{
			OutError = ObjectError;
			return false;
		}

		if (!ObjectValue->IsA(ObjectProperty->PropertyClass))
		{
			OutError = FString::Printf(TEXT("Object '%s' is not compatible with property class '%s'."), *ObjectValue->GetClass()->GetName(), *ObjectProperty->PropertyClass->GetName());
			return false;
		}

		ObjectProperty->SetObjectPropertyValue(ValuePtr, ObjectValue);
		return true;
	}

	if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		return SetStructValueFromJson(StructProperty->Struct, ValuePtr, JsonValue, OutError, MaxDepth);
	}

	if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		if (!FJsonValueHelper::TryGetArray(GetEnvelopeFieldOrSelf(JsonValue, TEXT("items")), Items) &&
			!FJsonValueHelper::TryGetArray(JsonValue, Items))
		{
			OutError = FString::Printf(TEXT("Failed to parse array value for property '%s'."), *Property->GetName());
			return false;
		}

		FScriptArrayHelper ArrayHelper(ArrayProperty, ValuePtr);
		ArrayHelper.Resize(Items.Num());
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			if (!SetPropertyValueFromJson(ArrayProperty->Inner, ArrayHelper.GetRawPtr(Index), Items[Index], OutError, MaxDepth - 1))
			{
				OutError = FString::Printf(TEXT("Array element %d: %s"), Index, *OutError);
				return false;
			}
		}

		return true;
	}

	if (FSetProperty* SetProperty = CastField<FSetProperty>(Property))
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		if (!FJsonValueHelper::TryGetArray(GetEnvelopeFieldOrSelf(JsonValue, TEXT("items")), Items) &&
			!FJsonValueHelper::TryGetArray(JsonValue, Items))
		{
			OutError = FString::Printf(TEXT("Failed to parse set value for property '%s'."), *Property->GetName());
			return false;
		}

		FScriptSetHelper SetHelper(SetProperty, ValuePtr);
		SetHelper.EmptyElements();
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			const int32 NewIndex = SetHelper.AddDefaultValue_Invalid_NeedsRehash();
			if (!SetPropertyValueFromJson(SetProperty->ElementProp, SetHelper.GetElementPtr(NewIndex), Items[Index], OutError, MaxDepth - 1))
			{
				OutError = FString::Printf(TEXT("Set element %d: %s"), Index, *OutError);
				return false;
			}
		}
		SetHelper.Rehash();
		return true;
	}

	if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
	{
		TArray<TSharedPtr<FJsonValue>> Entries;
		if (!FJsonValueHelper::TryGetArray(GetEnvelopeFieldOrSelf(JsonValue, TEXT("entries")), Entries))
		{
			OutError = FString::Printf(TEXT("Failed to parse map entries for property '%s'."), *Property->GetName());
			return false;
		}

		FScriptMapHelper MapHelper(MapProperty, ValuePtr);
		MapHelper.EmptyValues(Entries.Num());
		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			TSharedPtr<FJsonObject> EntryObject;
			if (!FJsonValueHelper::TryGetObject(Entries[Index], EntryObject) || !EntryObject.IsValid())
			{
				OutError = FString::Printf(TEXT("Map entry %d is not an object."), Index);
				return false;
			}

			const int32 NewIndex = MapHelper.AddDefaultValue_Invalid_NeedsRehash();
			if (!SetPropertyValueFromJson(MapProperty->KeyProp, MapHelper.GetKeyPtr(NewIndex), EntryObject->TryGetField(TEXT("key")), OutError, MaxDepth - 1))
			{
				OutError = FString::Printf(TEXT("Map key %d: %s"), Index, *OutError);
				return false;
			}

			if (!SetPropertyValueFromJson(MapProperty->ValueProp, MapHelper.GetValuePtr(NewIndex), EntryObject->TryGetField(TEXT("value")), OutError, MaxDepth - 1))
			{
				OutError = FString::Printf(TEXT("Map value %d: %s"), Index, *OutError);
				return false;
			}
		}
		MapHelper.Rehash();
		return true;
	}

	if (FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
	{
		FString ParsedValue;
		if (!GetStringLikeValue(JsonValue, ParsedValue))
		{
			TSharedPtr<FJsonObject> ObjectValue;
			if (FJsonValueHelper::TryGetObject(JsonValue, ObjectValue) && ObjectValue.IsValid())
			{
				ObjectValue->TryGetStringField(TEXT("object_path"), ParsedValue);
			}
		}

		if (ParsedValue.IsEmpty() && JsonValue.IsValid() && JsonValue->Type != EJson::Null)
		{
			OutError = FString::Printf(TEXT("Failed to parse soft object value for property '%s'."), *Property->GetName());
			return false;
		}

		const TCHAR* ImportResult = SoftObjectProperty->ImportText_Direct(*ParsedValue, ValuePtr, nullptr, PPF_None);
		if (!ImportResult)
		{
			OutError = FString::Printf(TEXT("Failed to set soft object property '%s' from '%s'."), *Property->GetName(), *ParsedValue);
			return false;
		}
		return true;
	}

	if (FSoftClassProperty* SoftClassProperty = CastField<FSoftClassProperty>(Property))
	{
		FString ParsedValue;
		if (!GetStringLikeValue(JsonValue, ParsedValue))
		{
			TSharedPtr<FJsonObject> ObjectValue;
			if (FJsonValueHelper::TryGetObject(JsonValue, ObjectValue) && ObjectValue.IsValid())
			{
				ObjectValue->TryGetStringField(TEXT("class_path"), ParsedValue);
			}
		}

		if (ParsedValue.IsEmpty() && JsonValue.IsValid() && JsonValue->Type != EJson::Null)
		{
			OutError = FString::Printf(TEXT("Failed to parse soft class value for property '%s'."), *Property->GetName());
			return false;
		}

		const TCHAR* ImportResult = SoftClassProperty->ImportText_Direct(*ParsedValue, ValuePtr, nullptr, PPF_None);
		if (!ImportResult)
		{
			OutError = FString::Printf(TEXT("Failed to set soft class property '%s' from '%s'."), *Property->GetName(), *ParsedValue);
			return false;
		}
		return true;
	}

	const FString FallbackImport = JsonValueToCompactString(JsonValue);
	const TCHAR* ImportResult = Property->ImportText_Direct(*FallbackImport, ValuePtr, nullptr, PPF_None);
	if (!ImportResult)
	{
		OutError = FString::Printf(TEXT("Unsupported or invalid value for property '%s' of type '%s'."), *Property->GetName(), *GetPropertyTypeString(Property));
		return false;
	}

	return true;
}

bool FVibeReflectionUtils::SetStructValueFromJson(
	const UScriptStruct* StructType,
	void* StructData,
	const TSharedPtr<FJsonValue>& JsonValue,
	FString& OutError,
	int32 MaxDepth)
{
	if (!StructType || !StructData)
	{
		OutError = TEXT("Invalid struct target.");
		return false;
	}

	if (MaxDepth <= 0)
	{
		OutError = TEXT("Maximum recursion depth reached while setting struct.");
		return false;
	}

	TSharedPtr<FJsonObject> StructObject;
	if (!FJsonValueHelper::TryGetObject(GetEnvelopeFieldOrSelf(JsonValue, TEXT("fields")), StructObject) || !StructObject.IsValid())
	{
		if (!FJsonValueHelper::TryGetObject(JsonValue, StructObject) || !StructObject.IsValid())
		{
			OutError = FString::Printf(TEXT("Failed to parse struct object for '%s'."), *StructType->GetName());
			return false;
		}
	}

	for (const auto& Pair : StructObject->Values)
	{
		if (Pair.Key.Equals(TEXT("kind"), ESearchCase::IgnoreCase) ||
			Pair.Key.Equals(TEXT("struct_path"), ESearchCase::IgnoreCase))
		{
			continue;
		}

		FProperty* FieldProperty = FindPropertyByName(const_cast<UScriptStruct*>(StructType), Pair.Key);
		if (!FieldProperty)
		{
			continue;
		}

		void* FieldValuePtr = FieldProperty->ContainerPtrToValuePtr<void>(StructData);
		if (!SetPropertyValueFromJson(FieldProperty, FieldValuePtr, Pair.Value, OutError, MaxDepth - 1))
		{
			OutError = FString::Printf(TEXT("Struct field '%s': %s"), *Pair.Key, *OutError);
			return false;
		}
	}

	return true;
}

bool FVibeReflectionUtils::SaveResolvedTarget(const FResolvedObjectTarget& Target, bool bCompileBlueprint, FString& OutError)
{
	UObject* SaveObject = Target.SaveObject ? Target.SaveObject : Target.TargetObject;
	if (!SaveObject)
	{
		return true;
	}

	if (UBlueprint* Blueprint = Cast<UBlueprint>(SaveObject))
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		if (bCompileBlueprint)
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
		}
		Blueprint->MarkPackageDirty();
		if (!UEditorAssetLibrary::SaveLoadedAsset(Blueprint, false))
		{
			OutError = FString::Printf(TEXT("Failed to save Blueprint asset '%s'."), *Blueprint->GetPathName());
			return false;
		}
		return true;
	}

	SaveObject->MarkPackageDirty();

	UPackage* Package = SaveObject->GetOutermost();
	if (!Package)
	{
		return true;
	}

	if (!SaveObject->IsAsset())
	{
		return true;
	}

	if (!UEditorAssetLibrary::SaveLoadedAsset(SaveObject, false))
	{
		OutError = FString::Printf(TEXT("Failed to save asset '%s'."), *SaveObject->GetPathName());
		return false;
	}

	return true;
}
