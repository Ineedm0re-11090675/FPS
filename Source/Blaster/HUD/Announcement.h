#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Announcement.generated.h"

UCLASS()
class BLASTER_API UAnnouncement : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (BindWidget))
	class UTextBlock* WarmUpTime;
	
	UPROPERTY(meta = (BindWidget))
	UTextBlock* AnnouncementText;

	UPROPERTY(meta = (BindWidget))
	UTextBlock* InfoText;

};
