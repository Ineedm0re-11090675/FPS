#pragma once


#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "CharacterOverlay.generated.h"

UCLASS()
class BLASTER_API UCharacterOverlay : public UUserWidget
{
	GENERATED_BODY()
public:
	virtual void NativeConstruct() override;

	void SetWeaponAmmoVisible(bool bVisible);

	UPROPERTY(meta = (BindWidget))
	class UProgressBar* HealthBar;

	UPROPERTY(meta = (BindWidget))
	class UTextBlock* HealthText;

	UPROPERTY(meta = (BindWidget))
	class UProgressBar* ShieldBar;

	UPROPERTY(meta = (BindWidget))
	class UTextBlock* ShieldText;
	
	UPROPERTY(meta = (BindWidget))
	UTextBlock* ScoreAmount;

	UPROPERTY(meta = (BindWidget))
	UTextBlock* DefeatAmount;

	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* AmmoText;

	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* Slash;
	
	UPROPERTY(meta = (BindWidget))
	UTextBlock* WeaponAmmoAmount;
	
	UPROPERTY(meta = (BindWidget))
	UTextBlock* CarriedAmmoAmount;

	UPROPERTY(meta = (BindWidget))
	UTextBlock* Timer;

	UPROPERTY(meta = (BindWidget))
	UTextBlock* GrenadeAmount;
	
	UPROPERTY(meta = (BindWidget))
	class UImage* GrenadeImage;

	UPROPERTY(meta = (BindWidget))
	UImage* HighPingWifiSignal;
	
	UPROPERTY(meta = (BindWidgetAnim),Transient)
	UWidgetAnimation* Shing;

	UPROPERTY(meta = (BindWidget))
	UTextBlock* BlueTeamTScore;

	UPROPERTY(meta = (BindWidget))
	UTextBlock* RedTeamTScore;

	UPROPERTY(meta = (BindWidget))
	UTextBlock* TeamSlash;
};
