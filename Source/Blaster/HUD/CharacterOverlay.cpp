#include "CharacterOverlay.h"

#include "Components/Image.h"
#include "Components/TextBlock.h"
void UCharacterOverlay::NativeConstruct()
{
	Super::NativeConstruct();

	SetWeaponAmmoVisible(false);
	if (HighPingWifiSignal)
	{
		HighPingWifiSignal->SetOpacity(0.f);
	}
}

void UCharacterOverlay::SetWeaponAmmoVisible(bool bVisible)
{
	const ESlateVisibility AmmoVisibility = bVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed;

	if (AmmoText)
	{
		AmmoText->SetVisibility(AmmoVisibility);
	}
	if (WeaponAmmoAmount)
	{
		WeaponAmmoAmount->SetVisibility(AmmoVisibility);
	}
	if (CarriedAmmoAmount)
	{
		CarriedAmmoAmount->SetVisibility(AmmoVisibility);
	}
	if (Slash)
	{
		Slash->SetVisibility(AmmoVisibility);
	}
	if (GrenadeImage)
	{
		GrenadeImage->SetVisibility(AmmoVisibility);
	}
	if (GrenadeAmount)
	{
		GrenadeAmount->SetVisibility(AmmoVisibility);
	}
}
