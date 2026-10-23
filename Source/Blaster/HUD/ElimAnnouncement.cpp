#include "ElimAnnouncement.h"

#include "Components/TextBlock.h"

void UElimAnnouncement::SetElimAnnouncementText(FString Attacker, FString Victim)
{
	FString ElimText = FString::Printf(TEXT("%s Elimmed %s"),*Attacker,*Victim);
	if (AnnouncementText)
	{
		AnnouncementText->SetText(FText::FromString(ElimText));
	}
}
