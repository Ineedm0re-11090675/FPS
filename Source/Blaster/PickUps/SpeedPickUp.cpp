#include "SpeedPickUp.h"
#include "Blaster/Character/MyBlasterCharacter.h"
#include "Blaster/BuffComponent/BuffComponent.h"

ASpeedPickUp::ASpeedPickUp()
{
	// Codex: Speed pickup uses Niagara only; hide the placeholder mesh that shows WorldGrid gray.
	SetPickupMeshVisible(false);
}

bool ASpeedPickUp::ShouldHidePickupMesh() const
{
	return true;
}

void ASpeedPickUp::OnSphereOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
                                  UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	Super::OnSphereOverlap(OverlappedComponent, OtherActor, OtherComp, OtherBodyIndex, bFromSweep, SweepResult);

	AMyBlasterCharacter* MyCharacter = Cast<AMyBlasterCharacter>(OtherActor);
	if (MyCharacter)
	{
		UBuffComponent* BuffComponent = MyCharacter->GetBuff();
		if (BuffComponent)
		{
			BuffComponent->BuffSpeed(BaseSpeedBuff, CrouchSpeedBuff, SpeedBuffTime);
		}
	}
	Destroy();
}
