// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "BlasterHUD.generated.h"

class UElimAnnouncement;

USTRUCT(BlueprintType)
struct FHUDPackage
{
	GENERATED_BODY()
public:	
	class UTexture2D* CrosshairCenter = nullptr;
	UTexture2D* CrosshairRight = nullptr;
	UTexture2D* CrosshairLeft = nullptr;
	UTexture2D* CrosshairUp = nullptr;
	UTexture2D* CrosshairBottom = nullptr;
	float SpreadSpeed = 0.f;
	FLinearColor CrosshairColor = FLinearColor::White;
};

UCLASS()
class BLASTER_API ABlasterHUD : public AHUD
{
	GENERATED_BODY()
public:
	//
	//去看源码会发现，这个是每帧自动调用的 
	//
	virtual void DrawHUD() override;
	UPROPERTY(EditAnywhere,Category = "Player Stats")
	TSubclassOf<class UUserWidget> CharacterOverlayClass;
	UPROPERTY()
	class UCharacterOverlay* CharacterOverlay;

	void AddCharacterOverlay();
	
	UPROPERTY(EditAnywhere,Category = "Announcement")
	TSubclassOf<UUserWidget> AnnouncementClass;

	UPROPERTY()
	class UAnnouncement* Announcement;

	void AddAnnouncement();

	void AddElimAnnouncement(FString Attacker,FString Victim);
protected:
	virtual void BeginPlay() override;
private:

	class APlayerController* OwningController;
	FHUDPackage HUDPackage;
	
	void DrawCrosshair(UTexture2D* Texture,FVector2D ViewPortCenter,FVector2D Spread);

	UPROPERTY(EditAnywhere)
	float SpreadSpeedMax=16.f;

	UPROPERTY(EditAnywhere)
	TSubclassOf<UElimAnnouncement>ElimAnnouncementClass;

	UPROPERTY(EditAnywhere)
	float ELimAnnouncementTime =2.f;

	UFUNCTION()
	void ElimAnnouncementTimerFinished(UElimAnnouncement* MsgToRemove);

	UPROPERTY()
	TArray<UElimAnnouncement*> ElimAnnouncements;
public:
	FORCEINLINE void SetHUDPackage(const FHUDPackage& HudPackage) { HUDPackage = HudPackage; }	
};
