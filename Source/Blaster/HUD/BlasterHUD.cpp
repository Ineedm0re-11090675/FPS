// Fill out your copyright notice in the Description page of Project Settings.


#include "BlasterHUD.h"
#include "CharacterOverlay.h"
#include "Announcement.h"
#include "ElimAnnouncement.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"

namespace
{
	TSubclassOf<UElimAnnouncement> ResolveElimAnnouncementClass(TSubclassOf<UElimAnnouncement> ConfiguredClass)
	{
		if (ConfiguredClass && ConfiguredClass != UElimAnnouncement::StaticClass())
		{
			return ConfiguredClass;
		}

		static TSubclassOf<UElimAnnouncement> DefaultElimAnnouncementClass =
			LoadClass<UElimAnnouncement>(nullptr, TEXT("/Game/BP/HUD/WBP_ElimAnnouncement.WBP_ElimAnnouncement_C"));

		return DefaultElimAnnouncementClass ? DefaultElimAnnouncementClass : ConfiguredClass;
	}
}

void ABlasterHUD::AddAnnouncement()
{
	APlayerController* PlayerController = GetOwningPlayerController();
	if (Announcement)
	{
		Announcement->SetVisibility(ESlateVisibility::Visible);
		return;
	}

	if (PlayerController && AnnouncementClass)
	{
		Announcement = CreateWidget<UAnnouncement>(PlayerController, AnnouncementClass);
		Announcement->AddToViewport();
	}
}

void ABlasterHUD::AddCharacterOverlay()
{
	APlayerController* PlayerController = GetOwningPlayerController();
	if (PlayerController == nullptr || CharacterOverlayClass == nullptr)
	{
		return;
	}

	if (CharacterOverlay)
	{
		if (!CharacterOverlay->IsInViewport())
		{
			CharacterOverlay->AddToViewport();
		}
		CharacterOverlay->SetVisibility(ESlateVisibility::Visible);
		return;
	}

	CharacterOverlay = CreateWidget<UCharacterOverlay>(PlayerController, CharacterOverlayClass);
	if (CharacterOverlay)
	{
		CharacterOverlay->AddToViewport();
	}
}

void ABlasterHUD::AddElimAnnouncement(FString Attacker,FString Victim)
{
	OwningController  =OwningController == nullptr? GetOwningPlayerController() : OwningController;
	TSubclassOf<UElimAnnouncement> WidgetClass = ResolveElimAnnouncementClass(ElimAnnouncementClass);
		
	if (OwningController && WidgetClass)
	{
		UElimAnnouncement* ElimAnnouncement = CreateWidget<UElimAnnouncement>(OwningController, WidgetClass);
		if(ElimAnnouncement)
		{
			ElimAnnouncement->SetElimAnnouncementText(Attacker,Victim);
			ElimAnnouncement->AddToViewport();

			for (UElimAnnouncement* Msg : ElimAnnouncements)
			{
				if (Msg && Msg->AnnouncementBox)
				{
					UCanvasPanelSlot* CanvaSlot = UWidgetLayoutLibrary::SlotAsCanvasSlot(Msg->AnnouncementBox);
					if (CanvaSlot)
					{
						FVector2D Position = CanvaSlot->GetPosition();
						FVector2D NewPosition(
							Position.X,
							Position.Y-CanvaSlot->GetSize().Y
							);
						CanvaSlot->SetPosition(NewPosition);
						
					}
				}
			}
			
			ElimAnnouncements.Add(ElimAnnouncement);
			FTimerHandle ElimTimerHandle;
			FTimerDelegate ElimTimerDelegate;
			
			ElimTimerDelegate.BindUFunction(this,"ElimAnnouncementTimerFinished",ElimAnnouncement);
			GetWorldTimerManager().SetTimer(
				ElimTimerHandle,
				ElimTimerDelegate,
				ELimAnnouncementTime,
				false
				);
		}
	}
}

void ABlasterHUD::BeginPlay()
{
	Super::BeginPlay();
}

void ABlasterHUD::DrawCrosshair(UTexture2D* Texture, FVector2D ViewPortCenter,FVector2D Spread)
{
	if (Texture == nullptr) return;

	const float TextHeight=Texture->GetSizeY();
	const float TextWidth=Texture->GetSizeX();
	const FVector2D TextureDrawPoint(
			ViewPortCenter.X-TextWidth/2+Spread.X,
			ViewPortCenter.Y-TextHeight/2+Spread.Y
	);
	DrawTexture(
		Texture,
		TextureDrawPoint.X,
		TextureDrawPoint.Y,
		TextWidth,
		TextHeight,
		0.f,
		0.f,
		1.f,
		1.f,
			HUDPackage.CrosshairColor
			);
}

void ABlasterHUD::DrawHUD()
{
	Super::DrawHUD();
	FVector2D ViewPortSize;
	float SpreadScale=HUDPackage.SpreadSpeed*SpreadSpeedMax;
	if (GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->GetViewportSize(ViewPortSize);
		const FVector2D ViewPortCenter(ViewPortSize.X/2.f, ViewPortSize.Y/2.f);

		if (HUDPackage.CrosshairCenter)
		{
			FVector2D Spread(0.f,0.f);
			DrawCrosshair(HUDPackage.CrosshairCenter,ViewPortCenter,Spread);
		}
		if (HUDPackage.CrosshairBottom)
		{
			FVector2D Spread(0.f,SpreadScale);
			DrawCrosshair(HUDPackage.CrosshairBottom,ViewPortCenter,Spread);
		}
		if (HUDPackage.CrosshairLeft)
		{
			FVector2D Spread(-SpreadScale,0.f);
			DrawCrosshair(HUDPackage.CrosshairLeft,ViewPortCenter,Spread);
		}
		if (HUDPackage.CrosshairRight)
		{
			FVector2D Spread(SpreadScale,0.f);
			DrawCrosshair(HUDPackage.CrosshairRight,ViewPortCenter,Spread);
		}
		if (HUDPackage.CrosshairUp)
		{
			FVector2D Spread(0.f,-SpreadScale);
			DrawCrosshair(HUDPackage.CrosshairUp,ViewPortCenter,Spread);
		}
	}
}

void ABlasterHUD::ElimAnnouncementTimerFinished(UElimAnnouncement* MsgToRemove)
{
	if (MsgToRemove)
	{
		MsgToRemove->RemoveFromParent();
	}
}
