#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "ElimAnnouncement.generated.h"

UCLASS()
class UElimAnnouncement : public UUserWidget
{
	GENERATED_BODY()

public:

	void SetElimAnnouncementText(FString Attacker,FString Victim);
	UPROPERTY(meta = (BindWidget))
	class UHorizontalBox* AnnouncementBox;
	
	UPROPERTY(meta = (BindWidget))
	class UTextBlock* AnnouncementText;
};
