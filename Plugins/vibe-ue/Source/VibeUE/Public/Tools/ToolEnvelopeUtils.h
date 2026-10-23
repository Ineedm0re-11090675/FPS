// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

struct VIBEUE_API FBatchToolEnvelope
{
	FString Operation;
	FString ResourceKind;
	TSharedPtr<FJsonObject> Target;

	int32 RequestedCount = 0;
	int32 SuccessCount = 0;
	int32 FailedCount = 0;

	TArray<TSharedPtr<FJsonValue>> Results;
	TArray<TSharedPtr<FJsonValue>> Errors;

	bool bIncludeSaveFields = false;
	bool bSaveRequested = false;
	bool bSaved = false;
	FString SaveError;

	bool bIncludeRestoreFields = false;
	bool bRestoreRequested = false;
	bool bRestoreSuccess = false;
	FString RestoreError;
};

class VIBEUE_API FToolEnvelopeUtils
{
public:
	static FString SerializeJsonObject(const TSharedPtr<FJsonObject>& JsonObject);

	static TSharedPtr<FJsonObject> BuildErrorEntry(
		const FString& IdentifierField,
		const FString& IdentifierValue,
		const FString& ErrorMessage,
		const FString& ErrorCode = FString());

	static TSharedPtr<FJsonObject> BuildBatchEnvelope(const FBatchToolEnvelope& Envelope);
};
